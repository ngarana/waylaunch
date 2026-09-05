#include "waylaunch/dropdown/dropdown_main.h"

#include "waylaunch/dropdown/dropdown_manager.h"
#include "waylaunch/dropdown/session_supervisor.h"
#include "waylaunch/subprocess.h"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

namespace waylaunch {
namespace {

void arm_timer(int timer_fd, std::chrono::milliseconds delay) {
    itimerspec spec{};
    spec.it_value.tv_sec = delay.count() / 1000;
    spec.it_value.tv_nsec = (delay.count() % 1000) * 1000000;
    timerfd_settime(timer_fd, 0, &spec, nullptr);
}

void disarm_timer(int timer_fd) {
    itimerspec spec{};
    timerfd_settime(timer_fd, 0, &spec, nullptr);
}

} // namespace

int dropdown_main(const std::string& slot) {
    SessionSupervisor supervisor(slot);
    DropdownManager manager;

    // Block the signals we multiplex through signalfd so they never run as
    // async handlers mid-fork.
    sigset_t mask{};
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) return 1;

    int signal_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (signal_fd < 0) return 1;
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_fd < 0) {
        close(signal_fd);
        return 1;
    }

    auto spawn = [&](std::chrono::steady_clock::time_point now) {
        std::vector<std::string> argv = SessionSupervisor::build_argv("", supervisor.app_id());
        pid_t pid = Subprocess::spawn_tracked(argv);
        if (pid > 0) {
            supervisor.note_spawned(pid, now);
            // Phase 1 owns the process but not placement yet (phase 2 wires
            // j/clients observations). Assume hidden until shown.
            manager.process_event(DropdownEvent::WindowHidden);
        } else {
            // Fork failed: retry with backoff rather than hot-looping.
            auto delay = supervisor.note_exited(now);
            if (delay.has_value()) arm_timer(timer_fd, *delay);
        }
    };

    // First press appears immediately (gap-3 fix): boot the session on start.
    manager.process_event(DropdownEvent::Toggle);
    spawn(std::chrono::steady_clock::now());

    bool running = true;
    while (running) {
        pollfd fds[2]{};
        fds[0].fd = signal_fd;
        fds[0].events = POLLIN;
        fds[1].fd = timer_fd;
        fds[1].events = POLLIN;
        int n = poll(fds, 2, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if ((fds[0].revents & POLLIN) != 0) {
            // Both fds are nonblocking, so these drain loops terminate with
            // EAGAIN once pending events are consumed. Never drop the
            // NONBLOCK flags: a blocking read here would hang the daemon
            // after the last pending signal (seen live via wchan).
            signalfd_siginfo info{};
            while (read(signal_fd, &info, sizeof(info)) == static_cast<ssize_t>(sizeof(info))) {
                if (info.ssi_signo == static_cast<uint32_t>(SIGUSR1)) {
                    auto before = manager.current_state();
                    manager.process_event(DropdownEvent::Toggle);
                    // Toggle from absent boots a fresh session (the `exit`
                    // inside it → press again → new terminal case).
                    if (before == DropdownState::Absent &&
                        manager.current_state() == DropdownState::Spawning) {
                        spawn(std::chrono::steady_clock::now());
                    }
                    // Visible⇄Hidden placement lands in phase 2's backend.
                } else if (info.ssi_signo == static_cast<uint32_t>(SIGCHLD)) {
                    int status = 0;
                    pid_t waited = 0;
                    while ((waited = waitpid(-1, &status, WNOHANG)) > 0) {
                        if (waited == supervisor.child_pid()) {
                            auto now = std::chrono::steady_clock::now();
                            manager.process_event(DropdownEvent::ChildExited);
                            auto delay = supervisor.note_exited(now);
                            if (delay.has_value()) arm_timer(timer_fd, *delay);
                        }
                    }
                } else {
                    running = false;
                }
            }
        }
        if ((fds[1].revents & POLLIN) != 0) {
            uint64_t expirations = 0;
            while (read(timer_fd, &expirations, sizeof(expirations)) ==
                   static_cast<ssize_t>(sizeof(expirations))) {}
            // Backoff elapsed after a death: re-enter Spawning and fork again.
            if (manager.current_state() == DropdownState::Absent) {
                manager.process_event(DropdownEvent::Toggle);
            }
            if (manager.current_state() == DropdownState::Spawning) {
                spawn(std::chrono::steady_clock::now());
            } else {
                disarm_timer(timer_fd);
            }
        }
    }

    if (supervisor.has_child()) {
        kill(supervisor.child_pid(), SIGTERM);
        // Bounded reap: give the terminal a grace period, then escalate.
        // A blocking waitpid here would hang shutdown on a stubborn child.
        int status = 0;
        bool reaped = false;
        for (int i = 0; i < 50; ++i) {
            pid_t waited = waitpid(supervisor.child_pid(), &status, WNOHANG);
            if (waited != 0) {
                reaped = true;
                break;
            }
            usleep(10000);
        }
        if (!reaped) {
            kill(supervisor.child_pid(), SIGKILL);
            waitpid(supervisor.child_pid(), &status, 0);
        }
    }
    close(timer_fd);
    close(signal_fd);
    return 0;
}

} // namespace waylaunch
