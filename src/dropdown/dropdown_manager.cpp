#include "waylaunch/dropdown/dropdown_manager.h"

namespace waylaunch {

void DropdownManager::process_event(DropdownEvent event) {
    switch (state_) {
        case DropdownState::Absent:
            // Only a toggle boots the session; stray window/child events while
            // there is no child are ignored so a late SIGCHLD cannot resurrect
            // a dead slot.
            if (event == DropdownEvent::Toggle) state_ = DropdownState::Spawning;
            break;
        case DropdownState::Spawning:
            switch (event) {
                case DropdownEvent::Toggle:
                    // Toggle while spawning is idempotent: stay spawning rather
                    // than flapping back to absent and losing the child we just
                    // forked.
                    break;
                case DropdownEvent::WindowShown: state_ = DropdownState::Visible; break;
                case DropdownEvent::WindowHidden: state_ = DropdownState::Hidden; break;
                case DropdownEvent::WindowClosed:
                case DropdownEvent::ChildExited: state_ = DropdownState::Absent; break;
            }
            break;
        case DropdownState::Hidden:
            switch (event) {
                case DropdownEvent::Toggle:
                case DropdownEvent::WindowShown: state_ = DropdownState::Visible; break;
                case DropdownEvent::WindowClosed:
                case DropdownEvent::ChildExited: state_ = DropdownState::Absent; break;
                case DropdownEvent::WindowHidden: break;
            }
            break;
        case DropdownState::Visible:
            switch (event) {
                case DropdownEvent::Toggle:
                case DropdownEvent::WindowHidden: state_ = DropdownState::Hidden; break;
                case DropdownEvent::WindowClosed:
                case DropdownEvent::ChildExited: state_ = DropdownState::Absent; break;
                case DropdownEvent::WindowShown: break;
            }
            break;
    }
}

} // namespace waylaunch
