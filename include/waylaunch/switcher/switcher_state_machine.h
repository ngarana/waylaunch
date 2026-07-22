#pragma once

namespace waylaunch {

enum class SwitcherState {
    Hidden,
    ActiveHoldingMod,
    Dismissing
};

enum class SwitcherEvent {
    Trigger,
    Next,
    Prev,
    Jump,
    Quit,
    Minimize,
    Confirm,
    Cancel
};

class SwitcherStateMachine {
public:
    SwitcherState current_state() const { return state_; }
    
    bool is_active() const {
        return state_ == SwitcherState::ActiveHoldingMod;
    }

    void process_event(SwitcherEvent event);

private:
    SwitcherState state_ = SwitcherState::Hidden;
};

} // namespace waylaunch
