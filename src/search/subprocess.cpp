#include "waylaunch/subprocess.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace waylaunch {

ProcessResult Subprocess::run(const std::vector<std::string>& argv, const std::string& stdin_data) {
    if (argv.empty())
        return {.exit_code = -1, .stdout = "", .stderr = "Failed to spawn: empty argv"};

    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    auto close_pipe = [](int pipefd[2]) {
        if (pipefd[0] >= 0) close(pipefd[0]);
        if (pipefd[1] >= 0) close(pipefd[1]);
        pipefd[0] = pipefd[1] = -1;
    };
    auto close_all = [&]() {
        close_pipe(stdin_pipe);
        close_pipe(stdout_pipe);
        close_pipe(stderr_pipe);
    };
    auto pipe_failure = [&](int error) {
        close_all();
        return ProcessResult{.exit_code = -1,
                             .stdout = "",
                             .stderr = "Failed to create pipe: " + std::string(strerror(error))};
    };

    if (pipe(stdin_pipe) < 0) return pipe_failure(errno);
    if (pipe(stdout_pipe) < 0) return pipe_failure(errno);
    if (pipe(stderr_pipe) < 0) return pipe_failure(errno);

    posix_spawn_file_actions_t actions;
    int action_error = posix_spawn_file_actions_init(&actions);
    if (action_error != 0) {
        close_all();
        return {.exit_code = -1,
                .stdout = "",
                .stderr = "Failed to prepare spawn: " + std::string(strerror(action_error))};
    }
    auto add_action = [&](int result) {
        if (result == 0) return true;
        action_error = result;
        return false;
    };
    const bool actions_ready =
        add_action(posix_spawn_file_actions_addclose(&actions, stdin_pipe[0])) &&
        add_action(posix_spawn_file_actions_addclose(&actions, stdout_pipe[0])) &&
        add_action(posix_spawn_file_actions_addclose(&actions, stderr_pipe[0])) &&
        add_action(posix_spawn_file_actions_adddup2(&actions, stdin_pipe[1], STDIN_FILENO)) &&
        add_action(posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO)) &&
        add_action(posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1], STDERR_FILENO)) &&
        add_action(posix_spawn_file_actions_addclose(&actions, stdin_pipe[1])) &&
        add_action(posix_spawn_file_actions_addclose(&actions, stdout_pipe[1])) &&
        add_action(posix_spawn_file_actions_addclose(&actions, stderr_pipe[1]));
    if (!actions_ready) {
        posix_spawn_file_actions_destroy(&actions);
        close_all();
        return {.exit_code = -1,
                .stdout = "",
                .stderr = "Failed to prepare spawn: " + std::string(strerror(action_error))};
    }

    std::vector<char*> c_argv;
    c_argv.reserve(argv.size());
    for (const auto& s : argv) c_argv.push_back(const_cast<char*>(s.c_str()));
    c_argv.push_back(nullptr);

    pid_t pid;
    int ret = posix_spawnp(&pid, argv[0].c_str(), &actions, nullptr, c_argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);

    if (ret != 0) {
        close_all();
        return {.exit_code = -1,
                .stdout = "",
                .stderr = "Failed to spawn: " + std::string(strerror(ret))};
    }

    close(stdin_pipe[1]);
    stdin_pipe[1] = -1;
    close(stdout_pipe[1]);
    stdout_pipe[1] = -1;
    close(stderr_pipe[1]);
    stderr_pipe[1] = -1;
    if (stdin_data.empty()) {
        close(stdin_pipe[0]);
        stdin_pipe[0] = -1;
    }

    std::string stdout_buf;
    std::string stderr_buf;
    std::array<char, 4096> read_buf;
    std::size_t stdin_offset = 0;
    std::array<pollfd, 3> pfds = {{{.fd = stdin_data.empty() ? -1 : stdin_pipe[0],
                                    .events = static_cast<short>(stdin_data.empty() ? 0 : POLLOUT),
                                    .revents = 0},
                                   {.fd = stdout_pipe[0], .events = POLLIN, .revents = 0},
                                   {.fd = stderr_pipe[0], .events = POLLIN, .revents = 0}}};

    auto set_nonblocking = [](int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) return false;
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    };
    std::string stdin_error;
    if (pfds[0].fd >= 0 && !set_nonblocking(pfds[0].fd)) {
        stdin_error = "stdin fcntl: " + std::string(strerror(errno));
        close(pfds[0].fd);
        pfds[0].fd = -1;
    }
    // Nonblocking output reads let us drain both streams without ever waiting
    // on one pipe while the other is full.
    if (!set_nonblocking(pfds[1].fd)) pfds[1].events = POLLIN;
    if (!set_nonblocking(pfds[2].fd)) pfds[2].events = POLLIN;

    // A child may close stdin before all input is written. Ignore SIGPIPE for
    // this synchronous write window so the error can be returned instead of
    // terminating the launcher.
    struct sigaction ignore_sigpipe{};
    struct sigaction old_sigpipe{};
    sigemptyset(&ignore_sigpipe.sa_mask);
    ignore_sigpipe.sa_handler = SIG_IGN;
    const bool sigpipe_changed = sigaction(SIGPIPE, &ignore_sigpipe, &old_sigpipe) == 0;

    auto close_stdin = [&]() {
        if (pfds[0].fd >= 0) close(pfds[0].fd);
        pfds[0].fd = -1;
        pfds[0].events = 0;
    };
    auto read_output = [&](pollfd& pfd, std::string& output) {
        if (pfd.fd < 0 || !(pfd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) return;
        ssize_t n;
        do { n = read(pfd.fd, read_buf.data(), read_buf.size()); } while (n < 0 && errno == EINTR);
        if (n > 0) {
            output.append(read_buf.data(), static_cast<std::size_t>(n));
        } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            close(pfd.fd);
            pfd.fd = -1;
        }
    };

    while (true) {
        if (pfds[0].fd < 0 && pfds[1].fd < 0 && pfds[2].fd < 0) break;
        int poll_ret;
        do {
            poll_ret = poll(pfds.data(), pfds.size(), -1);
        } while (poll_ret < 0 && errno == EINTR);
        if (poll_ret < 0) {
            stdin_error = "poll: " + std::string(strerror(errno));
            close_stdin();
            if (pfds[1].fd >= 0) {
                close(pfds[1].fd);
                pfds[1].fd = -1;
            }
            if (pfds[2].fd >= 0) {
                close(pfds[2].fd);
                pfds[2].fd = -1;
            }
            kill(pid, SIGTERM);
            break;
        }

        if (pfds[0].fd >= 0 && pfds[0].revents) {
            if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                stdin_error = "stdin pipe closed before all input was written";
                close_stdin();
            } else if (pfds[0].revents & POLLOUT) {
                const std::size_t remaining = stdin_data.size() - stdin_offset;
                const std::size_t chunk =
                    std::min<std::size_t>(remaining, static_cast<const unsigned long>(64 * 1024));
                ssize_t n;
                do {
                    n = write(pfds[0].fd, stdin_data.data() + stdin_offset, chunk);
                } while (n < 0 && errno == EINTR);
                if (n > 0) {
                    stdin_offset += static_cast<std::size_t>(n);
                    if (stdin_offset == stdin_data.size()) close_stdin();
                } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    stdin_error = "stdin write: " + std::string(strerror(errno));
                    close_stdin();
                }
            }
        }
        read_output(pfds[1], stdout_buf);
        read_output(pfds[2], stderr_buf);
    }

    if (sigpipe_changed) sigaction(SIGPIPE, &old_sigpipe, nullptr);

    int status;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    int exit_code = waited == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (!stdin_error.empty()) {
        if (!stderr_buf.empty() && stderr_buf.back() != '\n') stderr_buf.push_back('\n');
        stderr_buf += stdin_error;
        stderr_buf.push_back('\n');
    }
    return {
        .exit_code = exit_code, .stdout = std::move(stdout_buf), .stderr = std::move(stderr_buf)};
}

bool Subprocess::command_exists(const std::string& command) {
    const char* path_env = getenv("PATH");
    if (!path_env) return false;
    std::string path_str(path_env);
    std::istringstream stream(path_str);
    std::string dir;
    while (std::getline(stream, dir, ':')) {
        std::filesystem::path full = std::filesystem::path(dir) / command;
        if (std::filesystem::exists(full) && std::filesystem::is_regular_file(full)) return true;
    }
    return false;
}

// Double-fork + setsid so the launched process outlives waylaunch and is
// reparented to init; the intermediate child is reaped immediately.
void Subprocess::spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty()) return;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() == 0) {
            std::vector<char*> c_argv;
            c_argv.reserve(argv.size() + 1);
            for (const auto& s : argv) c_argv.push_back(const_cast<char*>(s.c_str()));
            c_argv.push_back(nullptr);
            execvp(c_argv[0], c_argv.data());
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
}

// Single fork + setsid so the child stays ours for waitpid/SIGCHLD reaping.
// The caller owns respawn policy; this only forks and execs.
pid_t Subprocess::spawn_tracked(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        std::vector<char*> c_argv;
        c_argv.reserve(argv.size() + 1);
        for (const auto& s : argv) c_argv.push_back(const_cast<char*>(s.c_str()));
        c_argv.push_back(nullptr);
        execvp(c_argv[0], c_argv.data());
        _exit(127);
    }
    return pid;
}

} // namespace waylaunch
