#include "waylaunch/switcher/app_switcher_manager.h"
#include <algorithm>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <filesystem>

namespace waylaunch {

namespace {

std::string resolve_icon_name(const std::string& app_id) {
    if (app_id.empty()) return "";

    static std::unordered_map<std::string, std::string> cache;
    static std::vector<std::string> search_dirs;
    static bool dirs_ready = false;

    if (!dirs_ready) {
        const char* home = std::getenv("HOME");
        const char* xdg_data = std::getenv("XDG_DATA_HOME");
        std::string local_apps = std::string(home ? home : "") + "/.local/share/applications";
        std::string user_apps = std::string(xdg_data ? xdg_data : "") + "/applications";

        search_dirs = {
            "/usr/share/applications",
            "/usr/local/share/applications",
            user_apps,
            local_apps,
            "/usr/share/gnome/apps",
            "/usr/share/mate/applications",
        };
        dirs_ready = true;
    }

    auto cached = cache.find(app_id);
    if (cached != cache.end()) return cached->second;

    std::vector<std::string> candidates;
    const char* xdg_data_dirs = std::getenv("XDG_DATA_DIRS");
    if (xdg_data_dirs) {
        std::string_view v(xdg_data_dirs);
        size_t pos = 0;
        while (pos < v.size()) {
            size_t end = v.find(':', pos);
            if (end == std::string_view::npos) end = v.size();
            std::string dir(v.substr(pos, end - pos));
            if (!dir.empty()) {
                std::string apps = dir + "/applications";
                search_dirs.push_back(apps);
            }
            pos = (end < v.size()) ? end + 1 : end;
        }
    }

    auto make_candidates = [&](const std::string& id) -> std::vector<std::string> {
        std::vector<std::string> c;
        c.push_back(id + ".desktop");
        size_t last_dot = id.find_last_of('.');
        if (last_dot != std::string::npos) {
            std::string seg = id.substr(last_dot + 1);
            c.push_back(seg + ".desktop");
        }
        size_t first_dot = id.find('.');
        if (first_dot != std::string::npos) {
            std::string no_dots = id.substr(0, first_dot);
            c.push_back(no_dots + ".desktop");
        }
        std::string dashed = id;
        for (char& ch : dashed) if (ch == '.') ch = '-';
        c.push_back(dashed + ".desktop");
        std::string underscored = id;
        for (char& ch : underscored) if (ch == '.') ch = '_';
        c.push_back(underscored + ".desktop");

        std::sort(c.begin(), c.end());
        c.erase(std::unique(c.begin(), c.end()), c.end());
        return c;
    };

    auto find_desktop_file = [&](const std::vector<std::string>& cands) -> std::string {
        for (const auto& name : cands) {
            for (const auto& dir : search_dirs) {
                std::string path = dir + "/" + name;
                if (std::filesystem::exists(path)) return path;
            }
        }
        return {};
    };

    std::vector<std::string> candidates_list = make_candidates(app_id);
    std::string desktop_path = find_desktop_file(candidates_list);

    std::string result;
    if (!desktop_path.empty()) {
        std::ifstream file(desktop_path);
        if (file.is_open()) {
            std::string line;
            bool in_desktop_entry = false;
            while (std::getline(file, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                size_t start = line.find_first_not_of(" \t");
                if (start == std::string::npos) continue;
                line = line.substr(start);
                if (line.empty() || line[0] == '#') continue;
                if (line[0] == '[') {
                    in_desktop_entry = (line == "[Desktop Entry]");
                    continue;
                }
                if (!in_desktop_entry) continue;
                size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                if (line.substr(0, eq) == "Icon") {
                    result = line.substr(eq + 1);
                    break;
                }
            }
        }
    }

    cache[app_id] = result;
    return result;
}

} // namespace

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
            g.icon_name = win.icon_name.empty()
                ? resolve_icon_name(win.app_id)
                : win.icon_name;
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
