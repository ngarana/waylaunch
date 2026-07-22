#include "waylaunch/switcher/switcher_input_controller.h"
#include <xkbcommon/xkbcommon-keysyms.h>

namespace waylaunch {

// Bitmask for Super (Mod4) and Alt (Mod1) in libxkbcommon default layout
constexpr uint32_t MOD_ALT = (1 << 3);  // Mod1
constexpr uint32_t MOD_SUPER = (1 << 6); // Mod4

SwitcherInputController::SwitcherInputController(AppSwitcherManager* manager, wl_seat* seat)
    : manager_(manager), seat_(seat) {
    setup_dispatch_table();
}

void SwitcherInputController::setup_dispatch_table() {
    key_dispatch_table_ = {
        {XKB_KEY_Tab,          [this]() { manager_->navigate_next(); }},
        {XKB_KEY_ISO_Left_Tab, [this]() { manager_->navigate_prev(); }},
        {XKB_KEY_grave,        [this]() { manager_->navigate_prev(); }},
        {XKB_KEY_Left,         [this]() { manager_->navigate_prev(); }},
        {XKB_KEY_Right,        [this]() { manager_->navigate_next(); }},
        {XKB_KEY_q,            [this]() { manager_->quit_selected(); }},
        {XKB_KEY_Q,            [this]() { manager_->quit_selected(); }},
        {XKB_KEY_h,            [this]() { manager_->toggle_minimize_selected(); }},
        {XKB_KEY_H,            [this]() { manager_->toggle_minimize_selected(); }},
        {XKB_KEY_1,            [this]() { manager_->jump_to(0); }},
        {XKB_KEY_2,            [this]() { manager_->jump_to(1); }},
        {XKB_KEY_3,            [this]() { manager_->jump_to(2); }},
        {XKB_KEY_4,            [this]() { manager_->jump_to(3); }},
        {XKB_KEY_5,            [this]() { manager_->jump_to(4); }},
        {XKB_KEY_6,            [this]() { manager_->jump_to(5); }},
        {XKB_KEY_7,            [this]() { manager_->jump_to(6); }},
        {XKB_KEY_8,            [this]() { manager_->jump_to(7); }},
        {XKB_KEY_9,            [this]() { manager_->jump_to(8); }},
        {XKB_KEY_Escape,       [this]() { cancel(); }},
        // Explicit confirm — a fallback for when releasing the modifier isn't
        // observed (some compositor keyboard-grab configs), and a natural key.
        {XKB_KEY_Return,       [this]() { confirm(); }},
        {XKB_KEY_KP_Enter,     [this]() { confirm(); }},
        {XKB_KEY_space,        [this]() { confirm(); }},
    };
}

void SwitcherInputController::trigger() {
    if (!manager_) return;
    state_machine_.process_event(SwitcherEvent::Trigger);
    manager_->show();
}

void SwitcherInputController::cancel() {
    if (!manager_) return;
    state_machine_.process_event(SwitcherEvent::Cancel);
    manager_->cancel();
}

void SwitcherInputController::confirm() {
    if (!manager_) return;
    state_machine_.process_event(SwitcherEvent::Confirm);
    manager_->confirm_selection(seat_);
}

bool SwitcherInputController::handle_key(uint32_t keysym, bool pressed) {
    if (!state_machine_.is_active()) return false;
    if (!pressed) return true; // Consume key release events while switcher active

    auto it = key_dispatch_table_.find(keysym);
    if (it != key_dispatch_table_.end()) {
        it->second();
        return true;
    }

    return true; // Suppress unmapped key presses while overlay active
}

bool SwitcherInputController::handle_modifiers(uint32_t mods_depressed) {
    active_modifier_mask_ = mods_depressed;

    // Check if modifier key (Super or Alt) has been released
    bool holding_mod = (mods_depressed & MOD_SUPER) || (mods_depressed & MOD_ALT);
    
    if (state_machine_.is_active() && !holding_mod) {
        state_machine_.process_event(SwitcherEvent::Confirm);
        if (manager_) {
            manager_->confirm_selection(seat_);
        }
        return true;
    }

    return state_machine_.is_active();
}

} // namespace waylaunch
