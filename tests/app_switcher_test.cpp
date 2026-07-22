#include "waylaunch/switcher/toplevel_backend.h"
#include "waylaunch/switcher/app_switcher_manager.h"
#include "waylaunch/switcher/switcher_state_machine.h"
#include <cassert>
#include <iostream>
#include <algorithm>

namespace waylaunch {

class MockToplevelBackend : public IToplevelBackend {
public:
    bool init(wl_display*, wl_registry*) override { return true; }

    void add_observer(IToplevelObserver* obs) override {
        observers_.push_back(obs);
    }

    void remove_observer(IToplevelObserver* obs) override {
        observers_.erase(std::remove(observers_.begin(), observers_.end(), obs), observers_.end());
    }

    void activate(uintptr_t handle_id, wl_seat*) override {
        activated_handles_.push_back(handle_id);
        for (auto& w : windows_) {
            w.is_active = (w.handle_id == handle_id);
            if (w.is_active) {
                notify_updated(w);
            }
        }
    }

    void close(uintptr_t handle_id) override {
        closed_handles_.push_back(handle_id);
        windows_.erase(
            std::remove_if(windows_.begin(), windows_.end(), [handle_id](const ToplevelWindow& w) {
                return w.handle_id == handle_id;
            }),
            windows_.end()
        );
        notify_closed(handle_id);
    }

    void set_minimized(uintptr_t handle_id, bool minimize) override {
        minimized_calls_.push_back({handle_id, minimize});
        for (auto& w : windows_) {
            if (w.handle_id == handle_id) {
                w.is_minimized = minimize;
                notify_updated(w);
                break;
            }
        }
    }

    const std::vector<ToplevelWindow>& windows() const override {
        return windows_;
    }

    // Helper methods for test setup
    void add_mock_window(uintptr_t handle, const std::string& app_id, const std::string& title, bool active = false) {
        ToplevelWindow win;
        win.handle_id = handle;
        win.app_id = app_id;
        win.title = title;
        win.is_active = active;
        windows_.push_back(win);
        notify_created(win);
    }

    const std::vector<uintptr_t>& activated_handles() const { return activated_handles_; }
    const std::vector<uintptr_t>& closed_handles() const { return closed_handles_; }

private:
    void notify_created(const ToplevelWindow& w) {
        for (auto* obs : observers_) obs->on_window_created(w);
    }
    void notify_updated(const ToplevelWindow& w) {
        for (auto* obs : observers_) obs->on_window_updated(w);
    }
    void notify_closed(uintptr_t handle_id) {
        for (auto* obs : observers_) obs->on_window_closed(handle_id);
    }

    std::vector<IToplevelObserver*> observers_;
    std::vector<ToplevelWindow> windows_;
    std::vector<uintptr_t> activated_handles_;
    std::vector<uintptr_t> closed_handles_;
    struct MinimizedCall { uintptr_t handle; bool min; };
    std::vector<MinimizedCall> minimized_calls_;
};

} // namespace waylaunch

void test_app_switcher_mru_and_navigation() {
    using namespace waylaunch;
    MockToplevelBackend backend;
    AppSwitcherManager manager(&backend);

    // 1. Add windows: Firefox, Alacritty, Obsidian
    backend.add_mock_window(101, "firefox", "Mozilla Firefox", false);
    backend.add_mock_window(102, "alacritty", "Terminal", true);
    backend.add_mock_window(103, "obsidian", "Notes", false);

    assert(manager.app_groups().size() == 3);

    // 2. Open switcher -> selection defaults to index 1 (previous MRU app)
    manager.show();
    assert(manager.is_visible());

    // 3. Navigation test
    size_t initial_idx = manager.selected_index();
    manager.navigate_next();
    assert(manager.selected_index() == (initial_idx + 1) % 3);

    manager.navigate_prev();
    assert(manager.selected_index() == initial_idx);

    // 4. Direct Jump test
    manager.jump_to(2);
    assert(manager.selected_index() == 2);

    // 5. Quick Action: Quit selected app
    uintptr_t target_handle = manager.selected_group()->primary_handle();
    manager.quit_selected();
    assert(!backend.closed_handles().empty());
    assert(backend.closed_handles().back() == target_handle);

    std::cout << "[PASS] AppSwitcher MRU and navigation unit test clean!\n";
}

void test_state_machine() {
    using namespace waylaunch;
    SwitcherStateMachine fsm;

    assert(fsm.current_state() == SwitcherState::Hidden);
    assert(!fsm.is_active());

    fsm.process_event(SwitcherEvent::Trigger);
    assert(fsm.current_state() == SwitcherState::ActiveHoldingMod);
    assert(fsm.is_active());

    fsm.process_event(SwitcherEvent::Confirm);
    assert(fsm.current_state() == SwitcherState::Hidden);
    assert(!fsm.is_active());

    std::cout << "[PASS] Switcher state machine unit test clean!\n";
}

int main() {
    test_app_switcher_mru_and_navigation();
    test_state_machine();
    std::cout << "All Command+Tab unit tests passed successfully!\n";
    return 0;
}
