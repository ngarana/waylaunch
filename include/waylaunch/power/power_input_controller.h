#pragma once

#include "waylaunch/power/power_manager.h"
#include "waylaunch/power/power_state_machine.h"
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace waylaunch {

// Keysym dispatch only (≈ SwitcherInputController): owns the state machine and
// a grid dispatch table; while the confirmation dialog is open, only
// Return (confirm) and Esc (cancel) are valid — everything else is swallowed.
class PowerInputController {
public:
    explicit PowerInputController(PowerManager* manager);

    // Returns true if the event was consumed by the power overlay.
    bool handle_key(uint32_t keysym, bool pressed);

    // The countdown clock is an input source too: called periodically while the
    // dialog is open; auto-confirms once the counter expires. Time injectable
    // for deterministic tests.
    void tick(ConfirmDialog::Clock::time_point now = ConfirmDialog::Clock::now());

    bool is_active() const { return state_machine_.is_active(); }
    void trigger();

private:
    void setup_dispatch_table();
    void activate();   // Return/Space on the grid
    void confirm();    // Return inside the dialog
    void cancel();     // Esc (dialog → grid, grid → dismiss)

    using KeyActionHandler = std::function<void()>;
    std::unordered_map<uint32_t, KeyActionHandler> key_dispatch_table_;

    PowerManager* manager_ = nullptr;
    PowerStateMachine state_machine_;
};

} // namespace waylaunch
