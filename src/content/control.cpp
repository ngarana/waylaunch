#include "waylaunch/content/control.h"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace waylaunch::content {

namespace {

bool set_sockaddr(sockaddr_un& addr, const std::string& path) {
    if (path.size() >= sizeof(addr.sun_path)) return false;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    return true;
}

// Read a whole reply until EOF (or timeout). Small messages, so this is fine.
std::string read_all(int fd, int timeout_ms) {
    std::string out;
    char buf[4096];
    while (true) {
        pollfd pfd{fd, POLLIN, 0};
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) break;
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

} // namespace

ControlServer::ControlServer(std::string socket_path) : path_(std::move(socket_path)) {}
ControlServer::~ControlServer() { stop(); }

bool ControlServer::start(Handler handler) {
    handler_ = std::move(handler);
    fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) return false;

    sockaddr_un addr;
    if (!set_sockaddr(addr, path_)) { close(fd_); fd_ = -1; return false; }
    ::unlink(path_.c_str());   // clear a stale socket from a prior run
    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd_); fd_ = -1; return false;
    }
    ::chmod(path_.c_str(), 0600);   // owner-only (NFR7)
    if (listen(fd_, 8) != 0) { close(fd_); fd_ = -1; ::unlink(path_.c_str()); return false; }

    stop_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    stop_.store(false);
    thread_ = std::thread([this] { accept_loop(); });
    return true;
}

void ControlServer::stop() {
    if (fd_ < 0) return;
    stop_.store(true);
    if (stop_fd_ >= 0) { uint64_t one = 1; ssize_t w = write(stop_fd_, &one, sizeof(one)); (void)w; }
    if (thread_.joinable()) thread_.join();
    if (stop_fd_ >= 0) { close(stop_fd_); stop_fd_ = -1; }
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
    ::unlink(path_.c_str());
}

void ControlServer::accept_loop() {
    // One client at a time — control traffic is tiny.
    while (!stop_.load()) {
        pollfd pfds[2] = {{fd_, POLLIN, 0}, {stop_fd_, POLLIN, 0}};
        int pr = poll(pfds, 2, -1);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pfds[1].revents & POLLIN) break;
        if (!(pfds[0].revents & POLLIN)) continue;

        int cfd = accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) continue;

        std::string req = read_all(cfd, 1000);
        // first line = command, rest of line = argument
        std::string line = req.substr(0, req.find('\n'));
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        std::string cmd = line, arg;
        auto sp = line.find(' ');
        if (sp != std::string::npos) { cmd = line.substr(0, sp); arg = line.substr(sp + 1); }

        std::string reply = handler_ ? handler_(cmd, arg) : "error: no handler\n";
        if (!reply.empty() && reply.back() != '\n') reply.push_back('\n');
        ssize_t w = write(cfd, reply.data(), reply.size());
        (void)w;
        close(cfd);
    }
}

std::optional<std::string> control_send(const std::string& socket_path,
                                        const std::string& line, int timeout_ms) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return std::nullopt;
    sockaddr_un addr;
    if (!set_sockaddr(addr, socket_path)) { close(fd); return std::nullopt; }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return std::nullopt;
    }
    std::string msg = line;
    if (msg.empty() || msg.back() != '\n') msg.push_back('\n');
    ssize_t w = write(fd, msg.data(), msg.size());
    (void)w;
    ::shutdown(fd, SHUT_WR);   // signal end of request so the server can reply
    std::string reply = read_all(fd, timeout_ms);
    close(fd);
    return reply;
}

} // namespace waylaunch::content
