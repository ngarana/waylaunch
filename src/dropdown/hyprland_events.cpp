#include "waylaunch/dropdown/hyprland_events.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace waylaunch {
namespace {

constexpr int kMinBackoffMs = 250;
constexpr int kMaxBackoffMs = 8000;

std::string event_socket_path() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const char* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (runtime == nullptr || runtime[0] == '\0' || signature == nullptr || signature[0] == '\0') {
        return "";
    }
    return std::string(runtime) + "/hypr/" + signature + "/.socket2.sock";
}

} // namespace

std::vector<HyprEvent> EventLineParser::push(const char* data, size_t size) {
    std::vector<HyprEvent> events;
    buffer_.append(data, size);
    size_t start = 0;
    while (true) {
        size_t end = buffer_.find('\n', start);
        if (end == std::string::npos) break;
        std::string line = buffer_.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t sep = line.find(">>");
        if (sep != std::string::npos) {
            events.push_back({.name = line.substr(0, sep), .payload = line.substr(sep + 2)});
        }
        start = end + 1;
    }
    buffer_.erase(0, start);
    // Bound memory against a peer that never sends newlines.
    constexpr size_t kMaxBuffered = static_cast<size_t>(64) * 1024;
    if (buffer_.size() > kMaxBuffered) buffer_.clear();
    return events;
}

HyprlandEventStream::HyprlandEventStream() = default;

HyprlandEventStream::~HyprlandEventStream() {
    if (fd_ >= 0) close(fd_);
}

bool HyprlandEventStream::ensure_connected(std::chrono::steady_clock::time_point now) {
    if (fd_ >= 0) return true;
    if (now < next_retry_) return false;
    std::string path = event_socket_path();
    bool connected = false;
    if (!path.empty() && path.size() < sizeof(sockaddr_un::sun_path)) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd >= 0) {
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
            // Local unix connect completes immediately; EINPROGRESS cannot
            // usefully happen here, but tolerate it either way.
            if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 ||
                errno == EINPROGRESS) {
                fd_ = fd;
                backoff_ms_ = kMinBackoffMs;
                connected = true;
            } else {
                close(fd);
            }
        }
    }
    if (!connected) {
        next_retry_ = now + std::chrono::milliseconds(backoff_ms_);
        backoff_ms_ = std::min(backoff_ms_ * 2, kMaxBackoffMs);
    }
    return connected;
}

std::vector<HyprEvent> HyprlandEventStream::read_available() {
    std::vector<HyprEvent> events;
    if (fd_ < 0) return events;
    char buf[4096];
    while (true) {
        ssize_t n = read(fd_, buf, sizeof(buf));
        if (n > 0) {
            auto chunk = parser_.push(buf, static_cast<size_t>(n));
            events.insert(events.end(), std::make_move_iterator(chunk.begin()),
                          std::make_move_iterator(chunk.end()));
        } else if (n == 0) {
            // Peer closed (Hyprland restart): drop and back off.
            close(fd_);
            fd_ = -1;
            next_retry_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(backoff_ms_);
            backoff_ms_ = std::min(backoff_ms_ * 2, kMaxBackoffMs);
            break;
        } else {
            break; // EAGAIN/EWOULDBLOCK drained, or EINTR: poll re-fires
        }
    }
    return events;
}

} // namespace waylaunch
