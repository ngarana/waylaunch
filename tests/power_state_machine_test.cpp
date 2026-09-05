#include "waylaunch/power/power_state_machine.h"
#include <cassert>
#include <iostream>

using namespace waylaunch;

// Hidden → Active → ConfirmOpen → Dismissing (§9.2, as amended: cancelling a
// confirmation dismisses the overlay instead of returning to the picker).
void test_transitions() {
    PowerStateMachine fsm;
    assert(fsm.current_state() == PowerState::Hidden);
    assert(!fsm.is_active());

    fsm.process_event(PowerEvent::Trigger);
    assert(fsm.current_state() == PowerState::Active);
    assert(fsm.is_active());
    assert(!fsm.is_confirm_open());

    // Destructive selection opens the dialog.
    fsm.process_event(PowerEvent::OpenConfirm);
    assert(fsm.current_state() == PowerState::ConfirmOpen);
    assert(fsm.is_active());
    assert(fsm.is_confirm_open());

    // Confirming inside the dialog dismisses the overlay.
    fsm.process_event(PowerEvent::Execute);
    assert(fsm.current_state() == PowerState::Dismissing);
    assert(!fsm.is_active());

    fsm.process_event(PowerEvent::Dismissed);
    assert(fsm.current_state() == PowerState::Hidden);

    std::cout << "[PASS] transitions\n";
}

// Cancelling a confirmation dismisses outright — no bounce back to the picker.
void test_cancel_in_dialog_dismisses() {
    PowerStateMachine fsm;
    fsm.process_event(PowerEvent::Trigger);
    fsm.process_event(PowerEvent::OpenConfirm);
    assert(fsm.is_confirm_open());

    fsm.process_event(PowerEvent::Cancel);
    assert(fsm.current_state() == PowerState::Dismissing);
    assert(!fsm.is_active());
    assert(!fsm.is_confirm_open());

    std::cout << "[PASS] cancel in dialog dismisses\n";
}

void test_cancel_at_grid_dismisses() {
    PowerStateMachine fsm;
    fsm.process_event(PowerEvent::Trigger);
    fsm.process_event(PowerEvent::Cancel);
    assert(fsm.current_state() == PowerState::Dismissing);
    std::cout << "[PASS] cancel at grid dismisses\n";
}

void test_invalid_events_ignored() {
    PowerStateMachine fsm;
    fsm.process_event(PowerEvent::Execute); // nothing shown yet
    fsm.process_event(PowerEvent::OpenConfirm);
    assert(fsm.current_state() == PowerState::Hidden);

    fsm.process_event(PowerEvent::Trigger);
    fsm.process_event(PowerEvent::Trigger); // re-trigger while active: no-op
    assert(fsm.current_state() == PowerState::Active);
    std::cout << "[PASS] invalid events ignored\n";
}

int main() {
    test_transitions();
    test_cancel_in_dialog_dismisses();
    test_cancel_at_grid_dismisses();
    test_invalid_events_ignored();
    std::cout << "All power state machine tests passed!\n";
    return 0;
}
