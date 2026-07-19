#pragma once

// Control channel — the mdutil analog. A small line-based protocol over a Unix
// domain socket under $XDG_RUNTIME_DIR: `waylaunchctl` connects, sends one
// command line, reads a text reply, disconnects. Used for status / pause /
// resume / reindex / reconcile / exclude / shutdown (FR8). The query hot path
// does NOT go through here — the launcher reads the index directly (WAL).

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace waylaunch::content {

class ControlServer {
public:
    // Handler receives (command, argument) and returns the reply text.
    using Handler = std::function<std::string(const std::string& cmd, const std::string& arg)>;

    explicit ControlServer(std::string socket_path);
    ~ControlServer();

    bool start(Handler handler);   // bind + listen + accept thread
    void stop();

private:
    void accept_loop();

    std::string       path_;
    int               fd_ = -1;
    int               stop_fd_ = -1;
    std::thread       thread_;
    std::atomic<bool> stop_{false};
    Handler           handler_;
};

// Client: connect, send one command line, return the reply (nullopt if the
// daemon isn't reachable). Used by `waylaunchctl`.
std::optional<std::string> control_send(const std::string& socket_path,
                                        const std::string& line,
                                        int timeout_ms = 3000);

} // namespace waylaunch::content
