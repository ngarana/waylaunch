#include "waylaunch/power/power_state_machine.h"

namespace waylaunch {

void PowerStateMachine::process_event(PowerEvent event) {
    switch (state_) {
        case PowerState::Hidden:
            if (event == PowerEvent::Trigger) state_ = PowerState::Active;
            break;

        case PowerState::Active:
            if (event == PowerEvent::OpenConfirm)  state_ = PowerState::ConfirmOpen;
            else if (event == PowerEvent::Execute) state_ = PowerState::Dismissing;
            else if (event == PowerEvent::Cancel)  state_ = PowerState::Dismissing;
            break;

        case PowerState::ConfirmOpen:
            if (event == PowerEvent::Execute)     state_ = PowerState::Dismissing;
            else if (event == PowerEvent::Cancel) state_ = PowerState::Active;
            break;

        case PowerState::Dismissing:
            state_ = PowerState::Hidden;
            break;
    }
}

} // namespace waylaunch
