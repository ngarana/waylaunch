#include "waylaunch/subprocess.h"
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <sstream>

extern char** environ;

namespace waylaunch {

ProcessResult Subprocess::run(const std::vector<std::string>& argv, const std::string& stdin_data) {
    int stdin_pipe[2], stdout_pipe[2], stderr_pipe[2];
    pipe(stdin_pipe);
    pipe(stdout_pipe);
    pipe(stderr_pipe);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, stdin_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, stderr_pipe[0]);
    posix_spawn_file_actions_adddup2(&actions, stdin_pipe[1], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, stdin_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, stderr_pipe[1]);

    std::vector<char*> c_argv;
    for (auto& s : argv) c_argv.push_back(const_cast<char*>(s.c_str()));
    c_argv.push_back(nullptr);

    pid_t pid;
    int ret = posix_spawnp(&pid, argv[0].c_str(), &actions, nullptr, c_argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);

    if (ret != 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return {-1, "", "Failed to spawn: " + std::string(strerror(ret))};
    }

    close(stdin_pipe[1]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    if (!stdin_data.empty()) {
        write(stdin_pipe[0], stdin_data.c_str(), stdin_data.size());
    }
    close(stdin_pipe[0]);

    std::string stdout_buf, stderr_buf;
    std::array<char, 4096> read_buf;
    std::vector<pollfd> pfds = {
        {stdout_pipe[0], POLLIN, 0},
        {stderr_pipe[0], POLLIN, 0}
    };

    while (true) {
        int poll_ret = poll(pfds.data(), pfds.size(), -1);
        if (poll_ret <= 0) break;
        if (pfds[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(stdout_pipe[0], read_buf.data(), read_buf.size());
            if (n > 0) stdout_buf.append(read_buf.data(), n);
            else if (n == 0) pfds[0].fd = -1;
        }
        if (pfds[1].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(stderr_pipe[0], read_buf.data(), read_buf.size());
            if (n > 0) stderr_buf.append(read_buf.data(), n);
            else if (n == 0) pfds[1].fd = -1;
        }
        if (pfds[0].fd == -1 && pfds[1].fd == -1) break;
    }

    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    int status;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {exit_code, std::move(stdout_buf), std::move(stderr_buf)};
}

void Subprocess::kill(pid_t pid, int sig) {
    if (pid > 0) ::kill(pid, sig);
}

int Subprocess::wait(pid_t pid) {
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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

// PipeProcess
PipeProcess::PipeProcess() = default;
PipeProcess::~PipeProcess() { kill(); }

bool PipeProcess::start(const std::vector<std::string>& argv) {
    int stdin_pipe[2], stdout_pipe[2];
    pipe(stdin_pipe);
    pipe(stdout_pipe);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, stdin_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
    posix_spawn_file_actions_adddup2(&actions, stdin_pipe[1], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, stdin_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[1]);

    std::vector<char*> c_argv;
    for (auto& s : argv) c_argv.push_back(const_cast<char*>(s.c_str()));
    c_argv.push_back(nullptr);

    int ret = posix_spawnp(&pid_, argv[0].c_str(), &actions, nullptr, c_argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);

    if (ret != 0) {
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        return false;
    }

    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    stdin_fd_ = stdin_pipe[0];
    stdout_fd_ = stdout_pipe[1];
    return true;
}

void PipeProcess::write(const std::string& data) {
    std::lock_guard lock(mutex_);
    if (stdin_fd_ >= 0) ::write(stdin_fd_, data.c_str(), data.size());
}

void PipeProcess::close_stdin() {
    std::lock_guard lock(mutex_);
    if (stdin_fd_ >= 0) { close(stdin_fd_); stdin_fd_ = -1; }
}

std::string PipeProcess::read_stdout() {
    std::string result;
    std::array<char, 4096> buf;
    struct pollfd pfd;
    pfd.fd = stdout_fd_;
    pfd.events = POLLIN;
    while (true) {
        int ret = poll(&pfd, 1, 100);
        if (ret <= 0) break;
        ssize_t n = read(stdout_fd_, buf.data(), buf.size());
        if (n > 0) result.append(buf.data(), n);
        else if (n == 0) break;
    }
    return result;
}

void PipeProcess::wait() {
    if (pid_ > 0) { int status; waitpid(pid_, &status, 0); pid_ = -1; }
}

void PipeProcess::kill() {
    std::lock_guard lock(mutex_);
    if (pid_ > 0) { ::kill(pid_, SIGTERM); waitpid(pid_, nullptr, 0); pid_ = -1; }
    if (stdin_fd_ >= 0) { close(stdin_fd_); stdin_fd_ = -1; }
    if (stdout_fd_ >= 0) { close(stdout_fd_); stdout_fd_ = -1; }
}

bool PipeProcess::is_running() const {
    std::lock_guard lock(mutex_);
    if (pid_ <= 0) return false;
    int status;
    return waitpid(pid_, &status, WNOHANG) == 0;
}

int PipeProcess::exit_code() const {
    std::lock_guard lock(mutex_);
    if (pid_ <= 0) return -1;
    int status;
    pid_t ret = waitpid(pid_, &status, WNOHANG);
    if (ret == 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

} // namespace waylaunch
