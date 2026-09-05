#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <sys/types.h>

namespace waylaunch {

// Production ancestry check: walks the ppid chain in /proc. False when
// anything is unreadable.
bool is_descendant_process(int pid, int ancestor);

// Canonical form for comparing event payloads (`55ce...`) with client
// addresses (`0x55ce...`): lowercase, no `0x` prefix.
std::string normalize_address(const std::string& address);

// Phase 3 retract policy (docs/DROPDOWN_IMPLEMENTATION.md §5). Decides whether
// an `activewindowv2>>ADDR` event should hide a visible dropdown. The naive
// version fights itself, so two suppressions apply:
//
// (a) a grace window after show — showing the window generates focus events;
// (b) a menu or picker opened *by* the terminal is a different address but
//     must not retract: while a descendant of the terminal's pid holds focus,
//     stay open. `j/clients` gives the focused pid; ancestry walks
//     `/proc/<pid>/stat`.
//
// Pure except for the injectable ancestry check, so unit tests drive timing
// and kinship without a compositor or /proc.
class FocusGuard {
  public:
    // True when focused_pid is ancestor_pid or one of its descendants.
    // False on any error (fail-closed: unresolvable focus retracts).
    using AncestryFn = std::function<bool(int focused_pid, int ancestor_pid)>;

    void configure(bool hide_on_focus_loss, int grace_ms,
                   AncestryFn ancestry = &is_descendant_process);
    // Refresh slot identity whenever known (pid changes on respawn; address
    // resolves via find_by_address).
    void set_slot(pid_t pid, const std::string& address);
    void note_shown(std::chrono::steady_clock::time_point now);
    bool should_retract(std::chrono::steady_clock::time_point now,
                        const std::string& focused_address, int focused_pid);

  private:
    bool hide_on_focus_loss_ = true;
    int grace_ms_ = 150;
    AncestryFn ancestry_ = &is_descendant_process;
    pid_t slot_pid_ = -1;
    std::string slot_address_;
    bool has_shown_ = false;
    std::chrono::steady_clock::time_point last_shown_;
};

} // namespace waylaunch
