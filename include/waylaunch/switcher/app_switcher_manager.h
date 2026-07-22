#pragma once

#include "waylaunch/switcher/toplevel_backend.h"
#include "waylaunch/switcher/app_group.h"
#include <vector>
#include <memory>
#include <functional>

namespace waylaunch {

class AppSwitcherManager : public IToplevelObserver {
public:
    explicit AppSwitcherManager(IToplevelBackend* backend);
    ~AppSwitcherManager() override;

    // IToplevelObserver interface
    void on_window_created(const ToplevelWindow& window) override;
    void on_window_updated(const ToplevelWindow& window) override;
    void on_window_closed(uintptr_t handle_id) override;

    // Switcher controls
    void show();
    void hide();
    bool is_visible() const { return visible_; }

    void navigate_next();
    void navigate_prev();
    void jump_to(size_t index);
    
    void quit_selected();
    void toggle_minimize_selected();
    void confirm_selection(wl_seat* seat);
    void cancel();

    size_t selected_index() const { return selected_index_; }
    const std::vector<AppGroup>& app_groups() const { return groups_; }
    const AppGroup* selected_group() const;

    using ChangeCallback = std::function<void()>;
    void set_change_callback(ChangeCallback cb) { on_change_ = std::move(cb); }

    // false → one entry per window (individual instances) instead of per app.
    void set_group_by_app(bool v) { group_by_app_ = v; rebuild_groups(); }

private:
    void rebuild_groups();
    void touch_active_group(const std::string& app_id);
    void notify_change();

    IToplevelBackend* backend_ = nullptr;
    std::vector<AppGroup> groups_;
    size_t selected_index_ = 0;
    bool visible_ = false;
    bool group_by_app_ = true;
    uint64_t clock_counter_ = 0;
    ChangeCallback on_change_;
};

} // namespace waylaunch
