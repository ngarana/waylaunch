#include "waylaunch/power/power_manager.h"
#include "waylaunch/power/power_input_controller.h"
#include <xkbcommon/xkbcommon-keysyms.h>
#include <cassert>
#include <iostream>

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
    m.navigate_next();               // wraps
    assert(m.selected_index() == 0);
    m.navigate_prev();               // wraps backwards
    assert(m.selected_index() == 2);

    m.jump_to(1);
    assert(m.selected_index() == 1);
    m.jump_to(99);                   // out of range: ignored
    assert(m.selected_index() == 1);

    std::cout << "[PASS] navigation and jump\n";
}

void test_non_destructive_runs_immediately() {
    StubPowerBackend backend;
    PowerManager m(&backend);
    m.show();

    m.jump_to(0);                    // "lock"
    m.activate_selected();
    assert(!m.confirm_dialog().is_open());
    assert(!m.is_visible());         // hid itself
    assert(m.has_pending_action());  // command deferred until surface is gone
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

    m.jump_to(1);                    // "restart" (destructive)
    m.activate_selected();
    assert(m.confirm_dialog().is_open());
    assert(m.confirm_dialog().action().id == "restart");
    assert(m.is_visible());          // still up, waiting on the dialog
    assert(!m.has_pending_action());

    // Esc inside the dialog → back to the grid, nothing pending.
    m.cancel();
    assert(!m.confirm_dialog().is_open());
    assert(m.is_visible());

    // Esc at the grid → hide, still nothing pending (hide-on-cancel).
    m.cancel();
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

    m.confirm();                     // reject: no dialog open, no action captured
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

    m.jump_to(2);                    // "suspend" (destructive)
    m.activate_selected();
    assert(!m.confirm_dialog().is_open());   // global toggle skips the dialog
    assert(!m.is_visible());
    m.execute_pending();
    assert(backend.executed == (std::vector<std::string>{"suspend"}));

    std::cout << "[PASS] confirm_destructive=false skips dialog\n";
}

void test_input_controller_dispatch() {
    StubPowerBackend backend;
    PowerManager m(&backend);
    PowerInputController input(&m);

    assert(!input.handle_key(XKB_KEY_Return, true));   // inactive: not consumed

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
    assert(input.handle_key(XKB_KEY_x, true));         // unmapped: swallowed
    assert(m.selected_index() == 1);

    // Return on destructive → modal dialog: Tab must NOT reach the grid.
    assert(input.handle_key(XKB_KEY_Return, true));
    assert(m.confirm_dialog().is_open());
    assert(input.handle_key(XKB_KEY_Tab, true));
    assert(m.selected_index() == 1);

    // Esc: dialog → grid (still active), then grid → dismissed.
    assert(input.handle_key(XKB_KEY_Escape, true));
    assert(!m.confirm_dialog().is_open());
    assert(input.is_active());
    assert(input.handle_key(XKB_KEY_Escape, true));
    assert(!m.is_visible());
    assert(!input.is_active());
    assert(backend.executed.empty());

    std::cout << "[PASS] input controller dispatch\n";
}

void test_dialog_focus_navigation() {
    StubPowerBackend backend;
    PowerManager m(&backend);
    PowerInputController input(&m);

    input.trigger();
    input.handle_key(XKB_KEY_2, true);               // "restart" (destructive)
    input.handle_key(XKB_KEY_Return, true);
    assert(m.confirm_dialog().is_open());
    assert(m.confirm_dialog().focused_button() == ConfirmDialog::Button::Confirm);

    // ←/→/Tab move focus between the two buttons instead of reaching the grid.
    input.handle_key(XKB_KEY_Left, true);
    assert(m.confirm_dialog().focused_button() == ConfirmDialog::Button::Cancel);
    assert(m.selected_index() == 1);                 // grid untouched
    input.handle_key(XKB_KEY_Tab, true);
    assert(m.confirm_dialog().focused_button() == ConfirmDialog::Button::Confirm);
    input.handle_key(XKB_KEY_Right, true);
    assert(m.confirm_dialog().focused_button() == ConfirmDialog::Button::Cancel);

    // Return on a focused Cancel closes the dialog — nothing executes.
    input.handle_key(XKB_KEY_Return, true);
    assert(!m.confirm_dialog().is_open());
    assert(m.is_visible());                          // back at the grid
    assert(input.is_active());
    assert(!m.has_pending_action());
    assert(backend.executed.empty());

    // Reopen: focus reset to Confirm; Return executes.
    input.handle_key(XKB_KEY_Return, true);
    assert(m.confirm_dialog().focused_button() == ConfirmDialog::Button::Confirm);
    input.handle_key(XKB_KEY_Return, true);
    assert(!m.is_visible());
    m.execute_pending();
    assert(backend.executed == (std::vector<std::string>{"restart"}));

    std::cout << "[PASS] dialog focus navigation\n";
}

void test_countdown_auto_confirm() {
    using Clock = ConfirmDialog::Clock;
    StubPowerBackend backend;
    PowerManager m(&backend);
    m.set_countdown_seconds(30);
    PowerInputController input(&m);

    input.trigger();
    m.jump_to(1);                                    // "restart"
    input.handle_key(XKB_KEY_Return, true);
    assert(m.confirm_dialog().is_open());
    assert(m.confirm_dialog().has_countdown());      // config propagated

    Clock::time_point now = Clock::now();
    input.tick(now + std::chrono::seconds(29));      // still counting: no-op
    assert(m.confirm_dialog().is_open());

    input.tick(now + std::chrono::seconds(31));      // expired: auto-confirm
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

    input.tick(Clock::now() + std::chrono::hours(1));   // hidden: no-op
    assert(!m.is_visible());

    input.trigger();
    input.tick(Clock::now() + std::chrono::hours(1));   // grid, no dialog: no-op
    assert(m.is_visible());
    assert(backend.executed.empty());

    std::cout << "[PASS] tick ignored outside dialog\n";
}

int main() {
    test_navigation_and_jump();
    test_non_destructive_runs_immediately();
    test_destructive_gated_behind_dialog();
    test_dialog_confirm_executes();
    test_confirm_destructive_off();
    test_input_controller_dispatch();
    test_dialog_focus_navigation();
    test_countdown_auto_confirm();
    test_tick_ignored_outside_dialog();
    std::cout << "All power manager tests passed!\n";
    return 0;
}
