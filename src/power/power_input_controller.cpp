#include "waylaunch/power/power_input_controller.h"
#include <xkbcommon/xkbcommon-keysyms.h>

namespace waylaunch {

PowerInputController::PowerInputController(PowerManager* manager)
    : manager_(manager) {
    setup_dispatch_table();
}

void PowerInputController::setup_dispatch_table() {
    // One row of cards, so every directional key maps to prev/next — the same
    // mental model as the switcher (§4.3). 1…6 jump by index.
    key_dispatch_table_ = {
        {XKB_KEY_Left,         [this]() { manager_->navigate_prev(); }},
        {XKB_KEY_Up,           [this]() { manager_->navigate_prev(); }},
        {XKB_KEY_ISO_Left_Tab, [this]() { manager_->navigate_prev(); }},
        {XKB_KEY_Right,        [this]() { manager_->navigate_next(); }},
        {XKB_KEY_Down,         [this]() { manager_->navigate_next(); }},
        {XKB_KEY_Tab,          [this]() { manager_->navigate_next(); }},
        {XKB_KEY_Home,         [this]() { manager_->jump_to(0); }},
        {XKB_KEY_End,          [this]() {
            if (!manager_->actions().empty())
                manager_->jump_to(manager_->actions().size() - 1);
        }},
        {XKB_KEY_1,            [this]() { manager_->jump_to(0); }},
        {XKB_KEY_2,            [this]() { manager_->jump_to(1); }},
        {XKB_KEY_3,            [this]() { manager_->jump_to(2); }},
        {XKB_KEY_4,            [this]() { manager_->jump_to(3); }},
        {XKB_KEY_5,            [this]() { manager_->jump_to(4); }},
        {XKB_KEY_6,            [this]() { manager_->jump_to(5); }},
        {XKB_KEY_Return,       [this]() { activate(); }},
        {XKB_KEY_KP_Enter,     [this]() { activate(); }},
        {XKB_KEY_space,        [this]() { activate(); }},
        {XKB_KEY_Escape,       [this]() { cancel(); }},
    };
}

void PowerInputController::trigger() {
    if (!manager_) return;
    state_machine_.process_event(PowerEvent::Trigger);
    manager_->show();
}

void PowerInputController::activate() {
    manager_->activate_selected();
    // The manager decided: dialog opened (destructive) or action went pending.
    state_machine_.process_event(manager_->confirm_dialog().is_open()
                                     ? PowerEvent::OpenConfirm
                                     : PowerEvent::Execute);
}

void PowerInputController::confirm() {
    state_machine_.process_event(PowerEvent::Execute);
    manager_->confirm();
}

void PowerInputController::cancel() {
    state_machine_.process_event(PowerEvent::Cancel);
    manager_->cancel();
}

void PowerInputController::tick(ConfirmDialog::Clock::time_point now) {
    if (!manager_ || !state_machine_.is_confirm_open()) return;
    if (manager_->confirm_dialog().expired(now)) confirm();
}

bool PowerInputController::handle_key(uint32_t keysym, bool pressed) {
    if (!manager_ || !state_machine_.is_active()) return false;
    if (!pressed) return true;   // consume releases while the overlay is up

    // Modal dialog: confirm/cancel plus button-focus movement; Tab and arrows
    // must NOT reach the grid. Return/Space activate the *focused* button.
    if (state_machine_.is_confirm_open()) {
        switch (keysym) {
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter:
            case XKB_KEY_space:
                if (manager_->confirm_dialog().focused_button() ==
                    ConfirmDialog::Button::Cancel) cancel();
                else confirm();
                break;
            case XKB_KEY_Escape:
                cancel();
                break;
            case XKB_KEY_Left:
            case XKB_KEY_Right:
            case XKB_KEY_Tab:
            case XKB_KEY_ISO_Left_Tab:
                manager_->toggle_dialog_focus();
                break;
            default:
                break;   // swallowed — the dialog is modal
        }
        return true;
    }

    auto it = key_dispatch_table_.find(keysym);
    if (it != key_dispatch_table_.end()) it->second();
    return true;   // suppress unmapped keys while the overlay is active
}

} // namespace waylaunch
