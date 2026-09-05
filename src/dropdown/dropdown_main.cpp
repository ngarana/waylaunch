#include "waylaunch/dropdown/dropdown_main.h"

#include "waylaunch/dropdown/dropdown_manager.h"
#include "waylaunch/dropdown/geometry_policy.h"
#include "waylaunch/dropdown/hyprland_backend.h"
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

// How long to wait for a freshly spawned terminal to appear in j/clients:
// 25 attempts × 200ms ≈ 5s, then assume hidden and let later toggles resync.
constexpr int kAppearAttempts = 25;
constexpr std::chrono::milliseconds kAppearRetry{200};

} // namespace

int dropdown_main(const std::string& slot) {
    SessionSupervisor supervisor(slot);
    DropdownManager manager;
    HyprlandBackend backend(slot);
    DropdownConfig config; // defaults (top, 100x40); TOML wiring lands with §6
    bool backend_usable = backend.supports_geometry();

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

    // What the single timerfd is currently counting down for.
    enum class TimerPurpose {
        None,
        Respawn,    // backoff elapsed after a death: fork again
        AppearRetry // spawned window not yet in j/clients: look again
    };
    TimerPurpose timer_purpose = TimerPurpose::None;
    int appear_attempts = 0;

    auto arm = [&](TimerPurpose purpose, std::chrono::milliseconds delay) {
        timer_purpose = purpose;
        arm_timer(timer_fd, delay);
    };

    auto spawn = [&]() {
        std::vector<std::string> argv = SessionSupervisor::build_argv("", supervisor.app_id());
        pid_t pid = Subprocess::spawn_tracked(argv);
        auto now = std::chrono::steady_clock::now();
        if (pid > 0) {
            supervisor.note_spawned(pid, now);
            // The window takes ~100ms to appear; poll j/clients until it
            // does, then park it on the hidden workspace (initial Hidden).
            appear_attempts = 0;
            arm(TimerPurpose::AppearRetry, kAppearRetry);
        } else {
            // Fork failed: retry with backoff rather than hot-looping.
            auto delay = supervisor.note_exited(now);
            if (delay.has_value()) arm(TimerPurpose::Respawn, *delay);
        }
    };

    // Place the slot window per the recomputed-on-every-show policy (§5.2):
    // geometry derives from the focused monitor, so hotplugged outputs and
    // resolution changes need no restart.
    auto place_visible = [&]() {
        auto window = backend.find_window(supervisor.app_id());
        auto monitor = backend.focused_monitor();
        if (!window.has_value() || !monitor.has_value()) return false;
        return backend.show(*window, compute_geometry(*monitor, config));
    };

    auto park_hidden = [&]() {
        auto window = backend.find_window(supervisor.app_id());
        if (!window.has_value()) return false;
        return backend.hide(*window);
    };

    // Reconcile manager state with compositor truth. Used on toggle paths;
    // returns false when there is no window to act on.
    auto sync_presence = [&]() {
        if (backend_usable && backend.find_window(supervisor.app_id()).has_value()) return true;
        if (!supervisor.has_child() && manager.current_state() != DropdownState::Absent) {
            manager.process_event(DropdownEvent::WindowClosed);
        }
        return false;
    };

    auto on_toggle = [&]() {
        auto before = manager.current_state();
        manager.process_event(DropdownEvent::Toggle);
        if (!backend_usable) return; // degraded: lifecycle only, no placement
        if (before == DropdownState::Absent && manager.current_state() == DropdownState::Spawning) {
            spawn(); // the `exit` → press-again → fresh-terminal case
        } else if (manager.current_state() == DropdownState::Visible) {
            if (!sync_presence() || !place_visible()) {
                manager.process_event(DropdownEvent::WindowHidden); // revert
            }
        } else if (manager.current_state() == DropdownState::Hidden) {
            if (sync_presence() && !park_hidden()) {
                manager.process_event(DropdownEvent::WindowShown); // revert
            }
        }
    };

    // First press appears (gap-3 fix): boot the session on start.
    manager.process_event(DropdownEvent::Toggle);
    spawn();

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
                    on_toggle();
                    // Visible⇄Hidden placement is above; focus-loss retract
                    // lands in phase 3's event stream.
                } else if (info.ssi_signo == static_cast<uint32_t>(SIGCHLD)) {
                    int status = 0;
                    pid_t waited = 0;
                    while ((waited = waitpid(-1, &status, WNOHANG)) > 0) {
                        if (waited == supervisor.child_pid()) {
                            auto now = std::chrono::steady_clock::now();
                            manager.process_event(DropdownEvent::ChildExited);
                            auto delay = supervisor.note_exited(now);
                            if (delay.has_value()) arm(TimerPurpose::Respawn, *delay);
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
            // Consumed: disarm before branching; each path re-arms as needed.
            disarm_timer(timer_fd);
            if (timer_purpose == TimerPurpose::Respawn) {
                timer_purpose = TimerPurpose::None;
                // Backoff elapsed after a death: re-enter Spawning and fork.
                if (manager.current_state() == DropdownState::Absent) {
                    manager.process_event(DropdownEvent::Toggle);
                }
                if (manager.current_state() == DropdownState::Spawning) spawn();
            } else if (timer_purpose == TimerPurpose::AppearRetry) {
                // Fresh child not yet in j/clients: hide it onto the slot's
                // hidden workspace once it appears (initial Hidden).
                bool placed = false;
                if (backend_usable) {
                    auto window = backend.find_window(supervisor.app_id());
                    if (window.has_value() && park_hidden()) {
                        placed = true;
                        manager.process_event(DropdownEvent::WindowHidden);
                    }
                } else {
                    placed = true; // no compositor to observe; assume hidden
                    manager.process_event(DropdownEvent::WindowHidden);
                }
                if (!placed && manager.current_state() == DropdownState::Spawning) {
                    if (++appear_attempts < kAppearAttempts) {
                        arm(TimerPurpose::AppearRetry, kAppearRetry);
                    } else {
                        timer_purpose = TimerPurpose::None;
                        manager.process_event(DropdownEvent::WindowHidden);
                    }
                } else {
                    timer_purpose = TimerPurpose::None;
                }
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
