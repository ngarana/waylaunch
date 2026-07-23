#pragma once

#include "waylaunch/power/power_action.h"
#include "waylaunch/power/power_action_backend.h"
#include "waylaunch/power/confirm_dialog.h"
#include <functional>
#include <optional>
#include <vector>

namespace waylaunch {

// Selection + visibility + the pending-action handoff (≈ AppSwitcherManager).
// Depends on IPowerActionBackend only; owns no rendering or input knowledge.
class PowerManager {
public:
    explicit PowerManager(IPowerActionBackend* backend);

    void show();
    void hide();
    bool is_visible() const { return visible_; }

    void navigate_next();
    void navigate_prev();
    void jump_to(size_t index);

    size_t selected_index() const { return selected_index_; }
    const std::vector<PowerAction>& actions() const { return actions_; }
    const PowerAction* selected_action() const;

    // Return/Space on the grid. Destructive actions (with confirm_destructive
    // on) open the dialog; everything else becomes the pending action and the
    // overlay hides. The command runs later via execute_pending() — AFTER the
    // caller has destroyed the surface (§4.7: never leave the overlay visible
    // over the action).
    void activate_selected();
    void confirm();   // dialog Return: dialog's action → pending, hide
    void cancel();    // dialog Esc → back to grid; grid Esc → hide
    void toggle_dialog_focus();   // ←/→/Tab inside the dialog

    ConfirmDialog& confirm_dialog() { return dialog_; }
    const ConfirmDialog& confirm_dialog() const { return dialog_; }

    void set_confirm_destructive(bool v) { confirm_destructive_ = v; }
    // Auto-confirm countdown for the dialog, seconds; 0 disables (§4.4 counter).
    void set_countdown_seconds(int v) { countdown_seconds_ = v > 0 ? v : 0; }

    bool has_pending_action() const { return pending_.has_value(); }
    int execute_pending();   // backend->execute; -1 if nothing pending

    using ChangeCallback = std::function<void()>;
    void set_change_callback(ChangeCallback cb) { on_change_ = std::move(cb); }

private:
    void notify_change();

    IPowerActionBackend* backend_ = nullptr;
    std::vector<PowerAction> actions_;
    ConfirmDialog dialog_;
    std::optional<PowerAction> pending_;
    size_t selected_index_ = 0;
    bool visible_ = false;
    bool confirm_destructive_ = true;
    int countdown_seconds_ = 0;
    ChangeCallback on_change_;
};

} // namespace waylaunch
