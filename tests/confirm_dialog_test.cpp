#include "waylaunch/power/confirm_dialog.h"
#include <cassert>
#include <iostream>

using namespace waylaunch;

void test_default_closed() {
    ConfirmDialog d;
    assert(!d.is_open());
    assert(d.action().id.empty());   // no action captured yet
    std::cout << "[PASS] default closed\n";
}

void test_open_captures_action() {
    ConfirmDialog d;
    PowerAction a;
    a.id = "shutdown";
    a.name = "Shut Down";
    a.confirm_text = "Are you sure you want to shut down your computer now?";
    a.destructive = true;

    d.open(a);
    assert(d.is_open());
    assert(d.action().id == "shutdown");
    assert(d.action().name == "Shut Down");
    std::cout << "[PASS] open captures action\n";
}

void test_close_and_reopen() {
    ConfirmDialog d;
    PowerAction a;
    a.id = "restart";
    d.open(a);
    d.close();
    assert(!d.is_open());

    PowerAction b;
    b.id = "suspend";
    d.open(b);
    assert(d.is_open());
    assert(d.action().id == "suspend");   // latest action wins
    std::cout << "[PASS] close and reopen\n";
}

void test_focus_toggle() {
    ConfirmDialog d;
    PowerAction a;
    a.id = "shutdown";
    d.open(a);

    // Return confirms by default (§4.4) → Confirm holds initial focus.
    assert(d.focused_button() == ConfirmDialog::Button::Confirm);
    d.toggle_focus();
    assert(d.focused_button() == ConfirmDialog::Button::Cancel);
    d.toggle_focus();
    assert(d.focused_button() == ConfirmDialog::Button::Confirm);

    // Reopening resets focus to Confirm even if it was left on Cancel.
    d.toggle_focus();
    d.close();
    d.open(a);
    assert(d.focused_button() == ConfirmDialog::Button::Confirm);
    std::cout << "[PASS] focus toggle\n";
}

void test_countdown() {
    using Clock = ConfirmDialog::Clock;
    ConfirmDialog d;
    PowerAction a;
    a.id = "shutdown";

    Clock::time_point t0 = Clock::now();
    d.open(a, 30, t0);
    assert(d.has_countdown());
    assert(d.remaining_seconds(t0) == 30);
    assert(!d.expired(t0));

    Clock::time_point t1 = t0 + std::chrono::milliseconds(10500);
    assert(d.remaining_seconds(t1) == 20);           // ceil(19.5)
    double f = d.remaining_fraction(t1);
    assert(f > 0.62 && f < 0.68);                    // 19.5/30 = 0.65

    Clock::time_point t2 = t0 + std::chrono::seconds(31);
    assert(d.remaining_seconds(t2) == 0);            // clamped, never negative
    assert(d.remaining_fraction(t2) == 0.0);
    assert(d.expired(t2));

    d.close();
    assert(!d.expired(t2));                          // closed dialogs never expire
    std::cout << "[PASS] countdown\n";
}

void test_countdown_disabled() {
    using Clock = ConfirmDialog::Clock;
    ConfirmDialog d;
    PowerAction a;
    a.id = "restart";

    Clock::time_point t0 = Clock::now();
    d.open(a, 0, t0);                                // 0 = no countdown
    assert(!d.has_countdown());
    assert(d.remaining_seconds(t0 + std::chrono::hours(1)) == 0);
    assert(d.remaining_fraction(t0 + std::chrono::hours(1)) == 0.0);
    assert(!d.expired(t0 + std::chrono::hours(1)));  // never auto-confirms
    std::cout << "[PASS] countdown disabled\n";
}

int main() {
    test_default_closed();
    test_open_captures_action();
    test_close_and_reopen();
    test_focus_toggle();
    test_countdown();
    test_countdown_disabled();
    std::cout << "All confirm dialog tests passed!\n";
    return 0;
}
