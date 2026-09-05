#pragma once

#include "waylaunch/switcher/app_switcher_manager.h"
#include "waylaunch/switcher/switcher_state_machine.h"
#include <functional>
#include <unordered_map>

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
    void confirm(); // activate the selected app now (Enter / release fallback)

  private:
    void setup_dispatch_table();

    using KeyActionHandler = std::function<void()>;
    std::unordered_map<uint32_t, KeyActionHandler> key_dispatch_table_;

    AppSwitcherManager* manager_ = nullptr;
    wl_seat* seat_ = nullptr;
    SwitcherStateMachine state_machine_;
    uint32_t active_modifier_mask_ = 0;
    // Set once a `modifiers` event actually reports the trigger mod held during
    // this session. Confirm-on-release only fires on a held→released edge, never
    // on the first sample alone — the compositor's very first post-focus
    // `modifiers` report can arrive as all-zero (e.g. a fresh keyboard enter
    // racing the real key state), which would otherwise read as an instant
    // release and confirm before the user had any chance to select. (§ instant
    // self-confirm bug)
    bool mod_was_held_ = false;
};

} // namespace waylaunch
