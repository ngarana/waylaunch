#pragma once

#include "waylaunch/switcher/app_switcher_manager.h"
#include "waylaunch/switcher/switcher_state_machine.h"
#include <unordered_map>
#include <functional>

struct wl_seat;

namespace waylaunch {

class SwitcherInputController {
public:
    SwitcherInputController(AppSwitcherManager* manager, wl_seat* seat);

    // Returns true if event was consumed by the switcher
    bool handle_key(uint32_t keysym, bool pressed);
    bool handle_modifiers(uint32_t mods_depressed);

    bool is_active() const { return state_machine_.is_active(); }
    void trigger();
    void cancel();
    void confirm();   // activate the selected app now (Enter / release fallback)

private:
    void setup_dispatch_table();

    using KeyActionHandler = std::function<void()>;
    std::unordered_map<uint32_t, KeyActionHandler> key_dispatch_table_;

    AppSwitcherManager* manager_ = nullptr;
    wl_seat* seat_ = nullptr;
    SwitcherStateMachine state_machine_;
    uint32_t active_modifier_mask_ = 0;
};

} // namespace waylaunch
