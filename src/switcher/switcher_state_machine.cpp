#include "waylaunch/switcher/switcher_state_machine.h"

namespace waylaunch {

void SwitcherStateMachine::process_event(SwitcherEvent event) {
    switch (state_) {
        case SwitcherState::Hidden:
            if (event == SwitcherEvent::Trigger || event == SwitcherEvent::Next) {
                state_ = SwitcherState::ActiveHoldingMod;
            }
            break;

        case SwitcherState::ActiveHoldingMod:
            if (event == SwitcherEvent::Confirm || event == SwitcherEvent::Cancel) {
                state_ = SwitcherState::Hidden;
            }
            break;

        case SwitcherState::Dismissing:
            state_ = SwitcherState::Hidden;
            break;
    }
}

} // namespace waylaunch
