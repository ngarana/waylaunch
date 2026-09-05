#pragma once

namespace waylaunch {

// Phase 1 state machine from docs/DROPDOWN_IMPLEMENTATION.md §5.
// Pure logic: no I/O, no compositor, no timers. The owner (dropdown_main)
// feeds it pid/window observations and reads back the placement calls it
// should issue via IPlacementBackend. Slot id is threaded through from the
// start so phase 4 (named slots) stays parameterization, not surgery.
enum class DropdownState {
    Absent,   // no child process; supervisor should spawn
    Spawning, // child forked, window not yet visible in j/clients
    Hidden,   // window lives on the hidden special workspace
    Visible,  // window is on the focused monitor's active workspace
};

enum class DropdownEvent {
    Toggle,       // SIGUSR1 / second `waylaunch --dropdown` invocation
    WindowShown,  // backend now reports the slot window as visible
    WindowHidden, // backend now reports it hidden (e.g. moved away externally)
    WindowClosed, // closewindow for the slot address / window vanished
    ChildExited,  // SIGCHLD for the supervised terminal pid
};

class DropdownManager {
  public:
    DropdownState current_state() const { return state_; }
    bool is_visible() const { return state_ == DropdownState::Visible; }

    void process_event(DropdownEvent event);

  private:
    DropdownState state_ = DropdownState::Absent;
};

} // namespace waylaunch
