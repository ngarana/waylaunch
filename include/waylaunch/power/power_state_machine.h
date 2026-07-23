#pragma once

namespace waylaunch {

enum class PowerState {
    Hidden,
    Active,        // grid shown, selection navigable
    ConfirmOpen,   // modal confirmation dialog owns the keys
    Dismissing
};

enum class PowerEvent {
    Trigger,       // show the overlay
    OpenConfirm,   // destructive action selected → dialog
    Execute,       // action confirmed (or non-destructive activated)
    Cancel,        // Esc: dialog → grid, grid → dismiss
    Dismissed      // teardown complete
};

class PowerStateMachine {
public:
    PowerState current_state() const { return state_; }

    bool is_active() const {
        return state_ == PowerState::Active || state_ == PowerState::ConfirmOpen;
    }
    bool is_confirm_open() const { return state_ == PowerState::ConfirmOpen; }

    void process_event(PowerEvent event);

private:
    PowerState state_ = PowerState::Hidden;
};

} // namespace waylaunch
