#include "waylaunch/dropdown/focus_guard.h"

#include <cassert>
#include <iostream>
#include <unistd.h>

using namespace waylaunch;
using Clock = std::chrono::steady_clock;

FocusGuard test_guard() {
    FocusGuard guard;
    guard.configure(true, 150, [](int, int) { return false; });
    guard.set_slot(1000, "0xabc");
    return guard;
}

void test_stranger_retracts() {
    FocusGuard guard = test_guard();
    guard.note_shown(Clock::now() - std::chrono::seconds(5)); // long past grace
    assert(guard.should_retract(Clock::now(), "55ce00000000", 2000));
    std::cout << "[PASS] stranger retracts\n";
}

void test_own_address_keeps_open() {
    FocusGuard guard = test_guard();
    guard.note_shown(Clock::now() - std::chrono::seconds(5));
    // All spellings of the slot address compare equal.
    assert(!guard.should_retract(Clock::now(), "0xabc", 1000));
    assert(!guard.should_retract(Clock::now(), "abc", 1000));
    assert(!guard.should_retract(Clock::now(), "0XABC", 9999));
    std::cout << "[PASS] own address keeps open\n";
}

void test_grace_suppresses_show_events() {
    FocusGuard guard = test_guard();
    auto shown = Clock::now();
    guard.note_shown(shown);
    assert(!guard.should_retract(shown + std::chrono::milliseconds(50), "55ce00000000", 2000));
    assert(guard.should_retract(shown + std::chrono::milliseconds(500), "55ce00000000", 2000));
    std::cout << "[PASS] grace suppresses show events\n";
}

void test_descendant_suppressed() {
    FocusGuard guard;
    guard.configure(true, 150, [](int focused, int ancestor) {
        return focused == 2001 && ancestor == 1000; // a picker owned by us
    });
    guard.set_slot(1000, "0xabc");
    guard.note_shown(Clock::now() - std::chrono::seconds(5));
    assert(!guard.should_retract(Clock::now(), "55ce99999999", 2001));
    assert(guard.should_retract(Clock::now(), "55ce99999999", 2002));
    std::cout << "[PASS] descendant suppressed\n";
}

void test_unknown_pid_retracts() {
    // Fail-closed: an unresolvable focus target must not pin us open.
    FocusGuard guard = test_guard();
    guard.note_shown(Clock::now() - std::chrono::seconds(5));
    assert(guard.should_retract(Clock::now(), "55ce00000000", -1));
    std::cout << "[PASS] unknown pid retracts\n";
}

void test_disabled_never_retracts() {
    FocusGuard guard;
    guard.configure(false, 150, [](int, int) { return false; });
    guard.set_slot(1000, "0xabc");
    guard.note_shown(Clock::now() - std::chrono::seconds(5));
    assert(!guard.should_retract(Clock::now(), "55ce00000000", 2000));
    std::cout << "[PASS] disabled never retracts\n";
}

void test_normalize() {
    assert(normalize_address("0x55CE85FA59E0") == "55ce85fa59e0");
    assert(normalize_address("55ce85fa59e0") == "55ce85fa59e0");
    assert(normalize_address("").empty());
    std::cout << "[PASS] normalize\n";
}

void test_live_proc_ancestry() {
    // The real /proc walk, on real pids: self descends from our parent and
    // from itself; init does not descend from us.
    assert(is_descendant_process(getpid(), getppid()));
    assert(is_descendant_process(getpid(), getpid()));
    assert(!is_descendant_process(1, getpid()));
    assert(!is_descendant_process(-5, getpid()));
    std::cout << "[PASS] live proc ancestry\n";
}

int main() {
    test_stranger_retracts();
    test_own_address_keeps_open();
    test_grace_suppresses_show_events();
    test_descendant_suppressed();
    test_unknown_pid_retracts();
    test_disabled_never_retracts();
    test_normalize();
    test_live_proc_ancestry();
    std::cout << "focus_guard_test: all passed\n";
    return 0;
}
