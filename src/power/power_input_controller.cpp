#include "waylaunch/power/power_input_controller.h"
#include "waylaunch/power/power_layout.h"
#include <xkbcommon/xkbcommon-keysyms.h>

namespace waylaunch {

PowerInputController::PowerInputController(PowerManager* manager) : manager_(manager) {
    setup_dispatch_table();
}

void PowerInputController::setup_dispatch_table() {
    // One row of cards, so every directional key maps to prev/next — the same
    // mental model as the switcher (§4.3). 1…6 jump by index.
    key_dispatch_table_ = {
        {XKB_KEY_Left, [this]() { manager_->navigate_prev(); }},
        {XKB_KEY_Up, [this]() { manager_->navigate_prev(); }},
        {XKB_KEY_ISO_Left_Tab, [this]() { manager_->navigate_prev(); }},
        {XKB_KEY_Right, [this]() { manager_->navigate_next(); }},
        {XKB_KEY_Down, [this]() { manager_->navigate_next(); }},
        {XKB_KEY_Tab, [this]() { manager_->navigate_next(); }},
        {XKB_KEY_Home, [this]() { manager_->jump_to(0); }},
        {XKB_KEY_End,
         [this]() {
             if (!manager_->actions().empty()) manager_->jump_to(manager_->actions().size() - 1);
         }},
        {XKB_KEY_1, [this]() { manager_->jump_to(0); }},
        {XKB_KEY_2, [this]() { manager_->jump_to(1); }},
        {XKB_KEY_3, [this]() { manager_->jump_to(2); }},
        {XKB_KEY_4, [this]() { manager_->jump_to(3); }},
        {XKB_KEY_5, [this]() { manager_->jump_to(4); }},
        {XKB_KEY_6, [this]() { manager_->jump_to(5); }},
        {XKB_KEY_Return, [this]() { activate(); }},
        {XKB_KEY_KP_Enter, [this]() { activate(); }},
        {XKB_KEY_space, [this]() { activate(); }},
        {XKB_KEY_Escape, [this]() { cancel(); }},
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
    state_machine_.process_event(manager_->confirm_dialog().is_open() ? PowerEvent::OpenConfirm
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

void PowerInputController::handle_pointer_motion(double x, double y, int screen_w, int screen_h) {
    if (!manager_ || !state_machine_.is_active()) return;

    // Dialog: hover picks the button under the cursor (leaves it put otherwise).
    if (state_machine_.is_confirm_open()) {
        auto d = power_layout::dialog(screen_w, screen_h);
        if (d.confirm.contains(x, y)) manager_->set_dialog_focus(ConfirmDialog::Button::Confirm);
        else if (d.cancel.contains(x, y)) manager_->set_dialog_focus(ConfirmDialog::Button::Cancel);
        return;
    }

    // Grid: hover highlights the card under the cursor (same as arrowing to it).
    auto h = power_layout::hud(screen_w, screen_h, static_cast<int>(manager_->actions().size()));
    for (size_t i = 0; i < h.cards.size(); ++i) {
        if (h.cards[i].contains(x, y)) {
            manager_->jump_to(i);
            return;
        }
    }
}

void PowerInputController::handle_pointer_click(double x, double y, int screen_w, int screen_h) {
    if (!manager_ || !state_machine_.is_active()) return;

    // Dialog: click a button to press it; click anywhere off the card dismisses
    // (declining is one gesture, matching the launcher's click-outside).
    if (state_machine_.is_confirm_open()) {
        auto d = power_layout::dialog(screen_w, screen_h);
        if (d.confirm.contains(x, y)) confirm();
        else if (d.cancel.contains(x, y) || !d.card.contains(x, y)) cancel();
        return;
    }

    // Grid: click a card to select-and-activate it in one gesture; click off
    // the panel dismisses the overlay.
    auto h = power_layout::hud(screen_w, screen_h, static_cast<int>(manager_->actions().size()));
    for (size_t i = 0; i < h.cards.size(); ++i) {
        if (h.cards[i].contains(x, y)) {
            manager_->jump_to(i);
            activate();
            return;
        }
    }
    if (!h.panel.contains(x, y)) cancel();
}

bool PowerInputController::handle_key(uint32_t keysym, bool pressed) {
    if (!manager_ || !state_machine_.is_active()) return false;
    if (!pressed) return true; // consume releases while the overlay is up

    // Modal dialog: confirm/cancel plus button-focus movement; Tab and arrows
    // must NOT reach the grid. Return/Space activate the *focused* button.
    if (state_machine_.is_confirm_open()) {
        switch (keysym) {
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter:
            case XKB_KEY_space:
                if (manager_->confirm_dialog().focused_button() == ConfirmDialog::Button::Cancel)
                    cancel();
                else confirm();
                break;
            case XKB_KEY_Escape: cancel(); break;
            case XKB_KEY_Left:
            case XKB_KEY_Right:
            case XKB_KEY_Tab:
            case XKB_KEY_ISO_Left_Tab: manager_->toggle_dialog_focus(); break;
            default: break; // swallowed — the dialog is modal
        }
        return true;
    }

    auto it = key_dispatch_table_.find(keysym);
    if (it != key_dispatch_table_.end()) it->second();
    return true; // suppress unmapped keys while the overlay is active
}

} // namespace waylaunch
