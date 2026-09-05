#include "waylaunch/power/power_input_controller.h"
#include "waylaunch/power/power_layout.h"
#include "waylaunch/power/power_manager.h"
#include <cassert>
#include <iostream>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace waylaunch {

// Stub backend (≈ the switcher tests' MockToplevelBackend): records executions
// instead of spawning commands.
class StubPowerBackend : public IPowerActionBackend {
  public:
    StubPowerBackend() {
        auto add = [this](const char* id, bool destructive, const char* subtext = "") {
            PowerAction a;
            a.id = id;
            a.name = id;
            a.argv = {"true"};
            a.destructive = destructive;
            a.subtext = subtext;
            if (destructive) a.confirm_text = std::string("confirm ") + id;
            acts_.push_back(std::move(a));
        };
        add("lock", false);
        add("restart", true, "session ending");
        add("suspend", true);
    }

    const std::vector<PowerAction>& actions() const override { return acts_; }
    int execute(const PowerAction& a) override {
        executed.push_back(a.id);
        return 0;
    }

    std::vector<std::string> executed;

  private:
    std::vector<PowerAction> acts_;
};

} // namespace waylaunch

using namespace waylaunch;

void test_navigation_and_jump() {
    StubPowerBackend backend;
    PowerManager m(&backend);

    m.show();
    assert(m.is_visible());
    assert(m.selected_index() == 0);

    m.navigate_next();
    assert(m.selected_index() == 1);
    m.navigate_next();
    m.navigate_next(); // wraps
    assert(m.selected_index() == 0);
    m.navigate_prev(); // wraps backwards
    assert(m.selected_index() == 2);

    m.jump_to(1);
    assert(m.selected_index() == 1);
    m.jump_to(99); // out of range: ignored
    assert(m.selected_index() == 1);

    std::cout << "[PASS] navigation and jump\n";
}

void test_non_destructive_runs_immediately() {
    StubPowerBackend backend;
    PowerManager m(&backend);
    m.show();

    m.jump_to(0); // "lock"
    m.activate_selected();
    assert(!m.confirm_dialog().is_open());
    assert(!m.is_visible());        // hid itself
    assert(m.has_pending_action()); // command deferred until surface is gone
    assert(backend.executed.empty());

    assert(m.execute_pending() == 0);
    assert(backend.executed == (std::vector<std::string>{"lock"}));
    assert(!m.has_pending_action()); // one-shot
    assert(m.execute_pending() == -1);

    std::cout << "[PASS] non-destructive runs immediately\n";
}

void test_destructive_gated_behind_dialog() {
    StubPowerBackend backend;
    PowerManager m(&backend);
    m.show();

    m.jump_to(1); // "restart" (destructive)
    m.activate_selected();
    assert(m.confirm_dialog().is_open());
    assert(m.confirm_dialog().action().id == "restart");
    assert(m.is_visible()); // still up, waiting on the dialog
    assert(!m.has_pending_action());

    // Cancelling the dialog dismisses the whole overlay — it does NOT drop back
    // on the picker — and nothing is executed.
    m.cancel();
    assert(!m.confirm_dialog().is_open());
    assert(!m.is_visible());
    assert(!m.has_pending_action());
    assert(m.execute_pending() == -1);
    assert(backend.executed.empty());

    std::cout << "[PASS] destructive gated behind dialog\n";
}

void test_dialog_confirm_executes() {
    StubPowerBackend backend;
    PowerManager m(&backend);
    m.show();

    m.confirm(); // reject: no dialog open, no action captured
    assert(!m.has_pending_action());

    m.jump_to(1);
    m.activate_selected();
    m.confirm();
    assert(!m.is_visible());
    assert(m.has_pending_action());
    m.execute_pending();
    assert(backend.executed == (std::vector<std::string>{"restart"}));

    std::cout << "[PASS] dialog confirm executes\n";
}

void test_confirm_destructive_off() {
    StubPowerBackend backend;
    PowerManager m(&backend);
    m.set_confirm_destructive(false);
    m.show();

    m.jump_to(2); // "suspend" (destructive)
    m.activate_selected();
    assert(!m.confirm_dialog().is_open()); // global toggle skips the dialog
    assert(!m.is_visible());
    m.execute_pending();
    assert(backend.executed == (std::vector<std::string>{"suspend"}));

    std::cout << "[PASS] confirm_destructive=false skips dialog\n";
}

void test_input_controller_dispatch() {
    StubPowerBackend backend;
    PowerManager m(&backend);
    PowerInputController input(&m);

    assert(!input.handle_key(XKB_KEY_Return, true)); // inactive: not consumed

    input.trigger();
    assert(input.is_active());
    assert(m.is_visible());

    assert(input.handle_key(XKB_KEY_Tab, true));
    assert(m.selected_index() == 1);
    assert(input.handle_key(XKB_KEY_Left, true));
    assert(m.selected_index() == 0);
    assert(input.handle_key(XKB_KEY_End, true));
    assert(m.selected_index() == 2);
    assert(input.handle_key(XKB_KEY_2, true));
    assert(m.selected_index() == 1);
    assert(input.handle_key(XKB_KEY_x, true)); // unmapped: swallowed
    assert(m.selected_index() == 1);

    // Return on destructive → modal dialog: Tab must NOT reach the grid.
    assert(input.handle_key(XKB_KEY_Return, true));
    assert(m.confirm_dialog().is_open());
    assert(input.handle_key(XKB_KEY_Tab, true));
    assert(m.selected_index() == 1);

    // Esc inside the dialog dismisses the overlay outright (same as Cancel).
    assert(input.handle_key(XKB_KEY_Escape, true));
    assert(!m.confirm_dialog().is_open());
    assert(!m.is_visible());
    assert(!input.is_active());
    assert(backend.executed.empty());

    // Keys after dismissal are no longer ours to consume.
    assert(!input.handle_key(XKB_KEY_Escape, true));

    std::cout << "[PASS] input controller dispatch\n";
}

void test_dialog_focus_navigation() {
    StubPowerBackend backend;
    PowerManager m(&backend);
    PowerInputController input(&m);

    input.trigger();
    input.handle_key(XKB_KEY_2, true); // "restart" (destructive)
    input.handle_key(XKB_KEY_Return, true);
    assert(m.confirm_dialog().is_open());
    assert(m.confirm_dialog().focused_button() == ConfirmDialog::Button::Confirm);

    // ←/→/Tab move focus between the two buttons instead of reaching the grid.
    input.handle_key(XKB_KEY_Left, true);
    assert(m.confirm_dialog().focused_button() == ConfirmDialog::Button::Cancel);
    assert(m.selected_index() == 1); // grid untouched
    input.handle_key(XKB_KEY_Tab, true);
    assert(m.confirm_dialog().focused_button() == ConfirmDialog::Button::Confirm);
    input.handle_key(XKB_KEY_Right, true);
    assert(m.confirm_dialog().focused_button() == ConfirmDialog::Button::Cancel);

    // Return on a focused Cancel dismisses the overlay — nothing executes, and
    // the picker does not come back.
    input.handle_key(XKB_KEY_Return, true);
    assert(!m.confirm_dialog().is_open());
    assert(!m.is_visible());
    assert(!input.is_active());
    assert(!m.has_pending_action());
    assert(backend.executed.empty());

    std::cout << "[PASS] dialog focus navigation\n";
}

// A fresh overlay: focus starts on Confirm, so a plain Return still executes.
void test_dialog_confirm_focus_default() {
    StubPowerBackend backend;
    PowerManager m(&backend);
    PowerInputController input(&m);

    input.trigger();
    input.handle_key(XKB_KEY_2, true); // "restart" (destructive)
    input.handle_key(XKB_KEY_Return, true);
    assert(m.confirm_dialog().focused_button() == ConfirmDialog::Button::Confirm);

    input.handle_key(XKB_KEY_Return, true);
    assert(!m.is_visible());
    assert(m.has_pending_action());
    m.execute_pending();
    assert(backend.executed == (std::vector<std::string>{"restart"}));

    std::cout << "[PASS] dialog confirm focus default\n";
}

void test_countdown_auto_confirm() {
    using Clock = ConfirmDialog::Clock;
    StubPowerBackend backend;
    PowerManager m(&backend);
    m.set_countdown_seconds(30);
    PowerInputController input(&m);

    input.trigger();
    m.jump_to(1); // "restart"
    input.handle_key(XKB_KEY_Return, true);
    assert(m.confirm_dialog().is_open());
    assert(m.confirm_dialog().has_countdown()); // config propagated

    Clock::time_point now = Clock::now();
    input.tick(now + std::chrono::seconds(29)); // still counting: no-op
    assert(m.confirm_dialog().is_open());

    input.tick(now + std::chrono::seconds(31)); // expired: auto-confirm
    assert(!m.confirm_dialog().is_open());
    assert(!m.is_visible());
    assert(m.has_pending_action());
    m.execute_pending();
    assert(backend.executed == (std::vector<std::string>{"restart"}));

    std::cout << "[PASS] countdown auto-confirm\n";
}

void test_tick_ignored_outside_dialog() {
    using Clock = ConfirmDialog::Clock;
    StubPowerBackend backend;
    PowerManager m(&backend);
    m.set_countdown_seconds(1);
    PowerInputController input(&m);

    input.tick(Clock::now() + std::chrono::hours(1)); // hidden: no-op
    assert(!m.is_visible());

    input.trigger();
    input.tick(Clock::now() + std::chrono::hours(1)); // grid, no dialog: no-op
    assert(m.is_visible());
    assert(backend.executed.empty());

    std::cout << "[PASS] tick ignored outside dialog\n";
}

// --- Pointer support: hit-tests must land on the same rects the renderer draws
//     (both come from power_layout), so tests click the geometric centers. ---
static constexpr int SW = 1280, SH = 800;

static void center_of(const power_layout::Rect& r, double& x, double& y) {
    x = r.x + (r.w / 2.0);
    y = r.y + (r.h / 2.0);
}

void test_pointer_grid_hover_and_activate() {
    StubPowerBackend backend;
    PowerManager m(&backend);
    PowerInputController input(&m);
    input.trigger();

    auto h = power_layout::hud(SW, SH, static_cast<int>(m.actions().size()));
    assert(h.cards.size() == 3);

    // Hover the 3rd card → selection follows the cursor.
    double x;
    double y;
    center_of(h.cards[2], x, y);
    input.handle_pointer_motion(x, y, SW, SH);
    assert(m.selected_index() == 2);

    // Hover back to the 1st, then click it (non-destructive) → runs immediately.
    center_of(h.cards[0], x, y);
    input.handle_pointer_motion(x, y, SW, SH);
    assert(m.selected_index() == 0);
    input.handle_pointer_click(x, y, SW, SH);
    assert(!m.is_visible());
    assert(m.has_pending_action());
    m.execute_pending();
    assert(backend.executed == (std::vector<std::string>{"lock"}));

    std::cout << "[PASS] pointer grid hover and activate\n";
}

void test_pointer_click_destructive_opens_dialog() {
    StubPowerBackend backend;
    PowerManager m(&backend);
    PowerInputController input(&m);
    input.trigger();

    auto h = power_layout::hud(SW, SH, 3);
    double x;
    double y;
    center_of(h.cards[1], x, y); // "restart" (destructive)
    input.handle_pointer_click(x, y, SW, SH);
    assert(m.confirm_dialog().is_open());
    assert(m.confirm_dialog().action().id == "restart");
    assert(m.is_visible());
    assert(backend.executed.empty());

    std::cout << "[PASS] pointer click destructive opens dialog\n";
}

void test_pointer_dialog_buttons() {
    StubPowerBackend backend;
    PowerManager m(&backend);
    PowerInputController input(&m);
    input.trigger();
    m.jump_to(1);
    input.handle_key(XKB_KEY_Return, true); // open dialog for "restart"
    assert(m.confirm_dialog().is_open());

    auto d = power_layout::dialog(SW, SH);
    double x;
    double y;

    // Hover the Cancel button → focus moves to it (absolute, not toggle).
    center_of(d.cancel, x, y);
    input.handle_pointer_motion(x, y, SW, SH);
    assert(m.confirm_dialog().focused_button() == ConfirmDialog::Button::Cancel);
    // Hover Confirm → focus follows.
    center_of(d.confirm, x, y);
    input.handle_pointer_motion(x, y, SW, SH);
    assert(m.confirm_dialog().focused_button() == ConfirmDialog::Button::Confirm);

    // Click Confirm → executes and dismisses.
    input.handle_pointer_click(x, y, SW, SH);
    assert(!m.is_visible());
    m.execute_pending();
    assert(backend.executed == (std::vector<std::string>{"restart"}));

    std::cout << "[PASS] pointer dialog buttons\n";
}

void test_pointer_dialog_cancel_button_dismisses() {
    StubPowerBackend backend;
    PowerManager m(&backend);
    PowerInputController input(&m);
    input.trigger();
    m.jump_to(1);
    input.handle_key(XKB_KEY_Return, true);

    auto d = power_layout::dialog(SW, SH);
    double x;
    double y;
    center_of(d.cancel, x, y);
    input.handle_pointer_click(x, y, SW, SH);
    assert(!m.is_visible()); // dismissed, not back to the grid
    assert(!m.has_pending_action());
    assert(backend.executed.empty());

    std::cout << "[PASS] pointer dialog cancel button dismisses\n";
}

void test_pointer_click_outside_dismisses() {
    // Grid: a click off the panel dismisses the overlay.
    {
        StubPowerBackend backend;
        PowerManager m(&backend);
        PowerInputController input(&m);
        input.trigger();
        input.handle_pointer_click(2.0, 2.0, SW, SH); // top-left corner
        assert(!m.is_visible());
        assert(backend.executed.empty());
    }
    // Dialog: a click off the card also dismisses (declining, one gesture).
    {
        StubPowerBackend backend;
        PowerManager m(&backend);
        PowerInputController input(&m);
        input.trigger();
        m.jump_to(1);
        input.handle_key(XKB_KEY_Return, true);
        assert(m.confirm_dialog().is_open());
        input.handle_pointer_click(2.0, 2.0, SW, SH);
        assert(!m.is_visible());
        assert(backend.executed.empty());
    }
    std::cout << "[PASS] pointer click outside dismisses\n";
}

void test_pointer_ignored_when_inactive() {
    StubPowerBackend backend;
    PowerManager m(&backend);
    PowerInputController input(&m);
    // Never triggered: motion and clicks are inert.
    auto h = power_layout::hud(SW, SH, 3);
    double x;
    double y;
    center_of(h.cards[1], x, y);
    input.handle_pointer_motion(x, y, SW, SH);
    input.handle_pointer_click(x, y, SW, SH);
    assert(!m.is_visible());
    assert(backend.executed.empty());
    std::cout << "[PASS] pointer ignored when inactive\n";
}

int main() {
    test_navigation_and_jump();
    test_non_destructive_runs_immediately();
    test_destructive_gated_behind_dialog();
    test_dialog_confirm_executes();
    test_confirm_destructive_off();
    test_input_controller_dispatch();
    test_dialog_focus_navigation();
    test_dialog_confirm_focus_default();
    test_countdown_auto_confirm();
    test_tick_ignored_outside_dialog();
    test_pointer_grid_hover_and_activate();
    test_pointer_click_destructive_opens_dialog();
    test_pointer_dialog_buttons();
    test_pointer_dialog_cancel_button_dismisses();
    test_pointer_click_outside_dismisses();
    test_pointer_ignored_when_inactive();
    std::cout << "All power manager tests passed!\n";
    return 0;
}
