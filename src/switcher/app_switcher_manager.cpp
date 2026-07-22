#include "waylaunch/switcher/app_switcher_manager.h"
#include <algorithm>

namespace waylaunch {

AppSwitcherManager::AppSwitcherManager(IToplevelBackend* backend)
    : backend_(backend) {
    if (backend_) {
        backend_->add_observer(this);
        rebuild_groups();
    }
}

AppSwitcherManager::~AppSwitcherManager() {
    if (backend_) {
        backend_->remove_observer(this);
    }
}

void AppSwitcherManager::on_window_created(const ToplevelWindow& window) {
    rebuild_groups();
    if (window.is_active) {
        touch_active_group(window.app_id);
    }
    notify_change();
}

void AppSwitcherManager::on_window_updated(const ToplevelWindow& window) {
    rebuild_groups();
    if (window.is_active) {
        touch_active_group(window.app_id);
    }
    notify_change();
}

void AppSwitcherManager::on_window_closed(uintptr_t /*handle_id*/) {
    rebuild_groups();
    notify_change();
}

void AppSwitcherManager::show() {
    rebuild_groups();
    visible_ = true;
    // Default selection to 1 (previous MRU app), or 0 if only 1 app exists
    selected_index_ = (groups_.size() > 1) ? 1 : 0;
    notify_change();
}

void AppSwitcherManager::hide() {
    visible_ = false;
    notify_change();
}

void AppSwitcherManager::navigate_next() {
    if (groups_.empty()) return;
    selected_index_ = (selected_index_ + 1) % groups_.size();
    notify_change();
}

void AppSwitcherManager::navigate_prev() {
    if (groups_.empty()) return;
    selected_index_ = (selected_index_ + groups_.size() - 1) % groups_.size();
    notify_change();
}

void AppSwitcherManager::jump_to(size_t index) {
    if (index < groups_.size()) {
        selected_index_ = index;
        notify_change();
    }
}

void AppSwitcherManager::quit_selected() {
    const AppGroup* grp = selected_group();
    if (!grp || !backend_) return;
    
    // Close all windows belonging to the selected app group
    for (const auto& w : grp->windows) {
        backend_->close(w.handle_id);
    }
}

void AppSwitcherManager::toggle_minimize_selected() {
    const AppGroup* grp = selected_group();
    if (!grp || !backend_) return;

    bool target_minimize = !grp->is_all_minimized();
    for (const auto& w : grp->windows) {
        backend_->set_minimized(w.handle_id, target_minimize);
    }
}

void AppSwitcherManager::confirm_selection(wl_seat* seat) {
    const AppGroup* grp = selected_group();
    if (grp && backend_) {
        uintptr_t handle = grp->primary_handle();
        if (handle) {
            backend_->activate(handle, seat);
        }
    }
    hide();
}

void AppSwitcherManager::cancel() {
    hide();
}

const AppGroup* AppSwitcherManager::selected_group() const {
    if (selected_index_ < groups_.size()) {
        return &groups_[selected_index_];
    }
    return nullptr;
}

void AppSwitcherManager::rebuild_groups() {
    if (!backend_) return;

    const auto& win_list = backend_->windows();
    std::vector<AppGroup> new_groups;

    for (const auto& win : win_list) {
        // Group by app_id, or (group_by_app=false) one entry per window so
        // individual instances — e.g. the same app on different workspaces — are
        // separately selectable. The per-window key is unique by handle.
        std::string key = group_by_app_
            ? (win.app_id.empty() ? std::string("unknown") : win.app_id)
            : ("\x01w:" + std::to_string(win.handle_id));

        auto it = std::find_if(new_groups.begin(), new_groups.end(), [&](const AppGroup& g) {
            return g.app_id == key;
        });

        if (it != new_groups.end()) {
            it->windows.push_back(win);
        } else {
            AppGroup g;
            g.app_id = key;
            // Ungrouped entries show the window title so instances are
            // distinguishable; grouped entries show the app id.
            g.display_name = group_by_app_
                ? key
                : (win.title.empty() ? (win.app_id.empty() ? "window" : win.app_id) : win.title);
            g.icon_name = win.icon_name.empty() ? win.app_id : win.icon_name;
            g.windows.push_back(win);
            
            // Preserve existing MRU timestamp if present
            auto old_it = std::find_if(groups_.begin(), groups_.end(), [&](const AppGroup& og) {
                return og.app_id == key;
            });
            if (old_it != groups_.end()) {
                g.last_focused_time = old_it->last_focused_time;
            } else {
                g.last_focused_time = ++clock_counter_;
            }

            new_groups.push_back(g);
        }
    }

    // Keep the entry holding the currently-active window at the MRU head, so a
    // plain Alt+Tab lands on the previous one (works for grouped and per-window).
    for (auto& g : new_groups) {
        if (g.is_any_active()) { g.last_focused_time = ++clock_counter_; break; }
    }

    // Sort MRU: highest timestamp first
    std::sort(new_groups.begin(), new_groups.end(), [](const AppGroup& a, const AppGroup& b) {
        return a.last_focused_time > b.last_focused_time;
    });

    groups_ = std::move(new_groups);
    if (selected_index_ >= groups_.size() && !groups_.empty()) {
        selected_index_ = groups_.size() - 1;
    }
}

void AppSwitcherManager::touch_active_group(const std::string& app_id) {
    if (app_id.empty()) return;
    for (auto& g : groups_) {
        if (g.app_id == app_id) {
            g.last_focused_time = ++clock_counter_;
            break;
        }
    }
    std::sort(groups_.begin(), groups_.end(), [](const AppGroup& a, const AppGroup& b) {
        return a.last_focused_time > b.last_focused_time;
    });
}

void AppSwitcherManager::notify_change() {
    if (on_change_) {
        on_change_();
    }
}

} // namespace waylaunch
