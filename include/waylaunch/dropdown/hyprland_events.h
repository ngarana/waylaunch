#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace waylaunch {

// Phase 3 event source (docs/DROPDOWN_IMPLEMENTATION.md §5): the Hyprland
// `.socket2.sock` stream of `eventname>>payload` lines. The fd is exposed
// directly so the daemon's poll() owns it — never a background thread.
struct HyprEvent {
    std::string name;
    std::string payload;
};

// Incremental line framer, decoupled from any fd so recorded fixtures can
// drive it without a compositor. Feed arbitrary byte chunks; complete lines
// come back as events. Lines without a `>>` separator are dropped.
class EventLineParser {
  public:
    std::vector<HyprEvent> push(const char* data, size_t size);

  private:
    std::string buffer_;
};

class HyprlandEventStream {
  public:
    HyprlandEventStream();
    ~HyprlandEventStream();
    HyprlandEventStream(const HyprlandEventStream&) = delete;
    HyprlandEventStream& operator=(const HyprlandEventStream&) = delete;

    // Pollable event fd, or -1 while disconnected (poll ignores negative
    // fds, so the daemon can pass it through unconditionally).
    int poll_fd() const { return fd_; }

    // (Re)connect when down, honouring the reconnect backoff after Hyprland
    // restarts. Cheap to call every loop iteration.
    bool ensure_connected(std::chrono::steady_clock::time_point now);

    // Drain available bytes into parsed events. An EOF or read error marks
    // the stream down (next ensure_connected retries with backoff).
    std::vector<HyprEvent> read_available();

  private:
    int fd_ = -1;
    EventLineParser parser_;
    std::chrono::steady_clock::time_point next_retry_ =
        std::chrono::steady_clock::time_point::min();
    int backoff_ms_ = 250;
};

} // namespace waylaunch
