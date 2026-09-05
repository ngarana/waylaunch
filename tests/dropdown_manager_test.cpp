#include "waylaunch/dropdown/dropdown_manager.h"
#include "waylaunch/dropdown/session_supervisor.h"

#include <cassert>
#include <chrono>
#include <iostream>

using namespace waylaunch;
using Clock = std::chrono::steady_clock;

void test_initial_toggle_spawns() {
    DropdownManager manager;
    assert(manager.current_state() == DropdownState::Absent);
    manager.process_event(DropdownEvent::Toggle);
    assert(manager.current_state() == DropdownState::Spawning);
    std::cout << "[PASS] initial toggle spawns\n";
}

void test_spawning_observes_hidden_then_toggle_shows() {
    DropdownManager manager;
    manager.process_event(DropdownEvent::Toggle);
    manager.process_event(DropdownEvent::WindowHidden);
    assert(manager.current_state() == DropdownState::Hidden);
    manager.process_event(DropdownEvent::Toggle);
    assert(manager.current_state() == DropdownState::Visible);
    assert(manager.is_visible());
    std::cout << "[PASS] spawning observes hidden then toggle shows\n";
}

void test_double_toggle_idempotent() {
    DropdownManager manager;
    manager.process_event(DropdownEvent::Toggle);
    // Toggle while spawning must not flap back to absent and lose the child.
    manager.process_event(DropdownEvent::Toggle);
    assert(manager.current_state() == DropdownState::Spawning);
    manager.process_event(DropdownEvent::WindowHidden);
    manager.process_event(DropdownEvent::Toggle);
    manager.process_event(DropdownEvent::Toggle);
    assert(manager.current_state() == DropdownState::Hidden);
    assert(!manager.is_visible());
    std::cout << "[PASS] double toggle idempotent\n";
}

void test_toggle_round_trip() {
    DropdownManager manager;
    manager.process_event(DropdownEvent::Toggle);
    manager.process_event(DropdownEvent::WindowHidden);
    manager.process_event(DropdownEvent::Toggle);
    assert(manager.is_visible());
    manager.process_event(DropdownEvent::Toggle);
    assert(manager.current_state() == DropdownState::Hidden);
    manager.process_event(DropdownEvent::Toggle);
    assert(manager.current_state() == DropdownState::Visible);
    std::cout << "[PASS] toggle round trip\n";
}

void test_child_exit_returns_to_absent() {
    DropdownManager manager;
    manager.process_event(DropdownEvent::Toggle);
    manager.process_event(DropdownEvent::WindowHidden);
    manager.process_event(DropdownEvent::Toggle);
    assert(manager.is_visible());
    manager.process_event(DropdownEvent::ChildExited);
    assert(manager.current_state() == DropdownState::Absent);
    // A fresh toggle after `exit` boots a new session (gap-3 fix).
    manager.process_event(DropdownEvent::Toggle);
    assert(manager.current_state() == DropdownState::Spawning);
    std::cout << "[PASS] child exit returns to absent\n";
}

void test_window_closed_from_any_state() {
    for (DropdownState start :
         {DropdownState::Spawning, DropdownState::Hidden, DropdownState::Visible}) {
        DropdownManager manager;
        manager.process_event(DropdownEvent::Toggle);
        if (start == DropdownState::Hidden) {
            manager.process_event(DropdownEvent::WindowHidden);
        } else if (start == DropdownState::Visible) {
            manager.process_event(DropdownEvent::WindowHidden);
            manager.process_event(DropdownEvent::Toggle);
        }
        assert(manager.current_state() == start);
        manager.process_event(DropdownEvent::WindowClosed);
        assert(manager.current_state() == DropdownState::Absent);
    }
    std::cout << "[PASS] window closed from any state\n";
}

void test_backoff_grows_and_resets() {
    SessionSupervisor supervisor("term");
    assert(supervisor.app_id() == "waylaunch-drop-term");
    auto t0 = Clock::now();
    supervisor.note_spawned(1234, t0);
    // Quick death: minimum backoff, then doubling up to the 8s cap.
    auto d1 = supervisor.note_exited(t0 + std::chrono::milliseconds(100));
    assert(d1.has_value() && d1->count() == 250);
    supervisor.note_spawned(1235, t0);
    auto d2 = supervisor.note_exited(t0 + std::chrono::milliseconds(100));
    assert(d2.has_value() && d2->count() == 500);
    // A healthy session (>10s) resets the backoff to the minimum.
    supervisor.note_spawned(1236, t0);
    auto d3 = supervisor.note_exited(t0 + std::chrono::seconds(11));
    assert(d3.has_value() && d3->count() == 250);
    std::cout << "[PASS] backoff grows and resets\n";
}

void test_slot_threaded_app_id() {
    SessionSupervisor notes("notes");
    assert(notes.slot() == "notes");
    assert(notes.app_id() == "waylaunch-drop-notes");
    auto argv = SessionSupervisor::build_argv("", notes.app_id());
    assert(!argv.empty());
    bool carries_id = false;
    for (const auto& part : argv) {
        if (part == notes.app_id()) carries_id = true;
    }
    assert(carries_id);
    std::cout << "[PASS] slot threaded app id\n";
}

int main() {
    test_initial_toggle_spawns();
    test_spawning_observes_hidden_then_toggle_shows();
    test_double_toggle_idempotent();
    test_toggle_round_trip();
    test_child_exit_returns_to_absent();
    test_window_closed_from_any_state();
    test_backoff_grows_and_resets();
    test_slot_threaded_app_id();
    std::cout << "dropdown_manager_test: all passed\n";
    return 0;
}
