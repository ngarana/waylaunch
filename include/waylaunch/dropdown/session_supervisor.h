#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace waylaunch {

// Phase 1 session owner from docs/DROPDOWN_IMPLEMENTATION.md §5.
// Spawns `$TERMINAL --class waylaunch-drop-<slot>` (slot id threaded through
// from phase 1 so phase 4 is free), tracks the pid, and computes respawn
// delays with exponential backoff. Pure policy except for the actual fork,
// which goes through Subprocess::spawn_tracked so tests can drive timing
// without forking.
class SessionSupervisor {
  public:
    static constexpr int kMinBackoffMs = 250;
    static constexpr int kMaxBackoffMs = 8000;
    // A session living longer than this resets the backoff to the minimum.
    static constexpr int kHealthyLifetimeSec = 10;

    explicit SessionSupervisor(std::string slot = "default") : slot_(std::move(slot)) {}

    const std::string& slot() const { return slot_; }
    // Value passed as the terminal's app-id/class so the backend can find it.
    std::string app_id() const { return "waylaunch-drop-" + slot_; }

    bool has_child() const { return child_.has_value(); }
    pid_t child_pid() const { return child_.value_or(-1); }
    // Releases ownership of the tracked pid without touching the backoff
    // schedule. For closewindow-driven teardown (phase 3): the compositor
    // reports the window gone while the process may still be alive, and the
    // owner kills it directly rather than waiting for SIGCHLD.
    std::optional<pid_t> take_child() {
        auto taken = child_;
        child_.reset();
        return taken;
    }

    void note_spawned(pid_t pid, std::chrono::steady_clock::time_point now);
    // Returns the respawn delay after a death at `now`; nullopt means do not
    // respawn (respawn disabled). A healthy lifetime resets the backoff.
    std::optional<std::chrono::milliseconds> note_exited(std::chrono::steady_clock::time_point now,
                                                         bool respawn_enabled = true);
    void set_respawn_enabled(bool enabled) { respawn_enabled_ = enabled; }

    // Build the argv for the terminal probe list. Empty terminal argument
    // means $TERMINAL, then kitty/foot/alacritty/wezterm/gnome-terminal.
    static std::vector<std::string> build_argv(const std::string& terminal,
                                               const std::string& app_id);
    // Build argv from a `[[dropdown.slots]]` command string (phase 4).
    // Whitespace-split without shell processing; the terminal's class flags
    // are injected so the window stays findable by app-id. Empty falls back
    // to the probe list.
    static std::vector<std::string> build_slot_argv(const std::string& command,
                                                    const std::string& app_id);

  private:
    std::string slot_;
    std::optional<pid_t> child_;
    std::chrono::steady_clock::time_point spawned_at_;
    int backoff_ms_ = kMinBackoffMs;
    bool respawn_enabled_ = true;
};

} // namespace waylaunch
