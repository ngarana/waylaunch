#include "waylaunch/providers/app_provider.h"
#include "waylaunch/app_launcher.h"
#include "waylaunch/history.h"
#include "waylaunch/subprocess.h"
#include <algorithm>

namespace waylaunch {

namespace {
std::string to_lower(std::string s) {
    std::ranges::transform(s, s.begin(), ::tolower);
    return s;
}
} // namespace

std::vector<ListItem> AppProvider::query(const ProviderQuery& q) {
    std::vector<ListItem> out;
    if (!apps_) return out;
    for (const auto& e : apps_->search(q.text)) {
        ListItem it;
        it.kind = ItemKind::Application;
        it.name = e.name;
        it.path = e.exec;
        it.reveal_path = e.desktop_path; // right-click → reveal the .desktop file
        it.description = e.comment.empty() ? e.generic_name : e.comment;
        it.icon_name = e.icon;
        std::string n = to_lower(e.name);
        size_t pos = n.find(q.lower);
        if (pos == 0) it.score = 1000.0F - static_cast<float>(std::min<size_t>(n.size(), 500));
        else if (pos != std::string::npos)
            it.score = 600.0F - static_cast<float>(std::min<size_t>(pos, 300));
        else it.score = 50.0F; // matched via comment/category
        if (history_)
            it.score +=
                history_->frecency(e.desktop_path.empty() ? "app:" + e.name : e.desktop_path);
        out.push_back(std::move(it));
    }
    std::ranges::stable_sort(
        out, [](const ListItem& a, const ListItem& b) { return a.score > b.score; });
    if (out.size() > static_cast<size_t>(q.max_results)) out.resize(q.max_results);
    return out;
}

bool AppProvider::activate(const ListItem& it) {
    if (it.kind != ItemKind::Application) return false;
    // it.path is the .desktop Exec line — run it via a shell so arg/env prefixes
    // work. It is a command, not a file.
    if (!it.path.empty()) Subprocess::spawn_detached({"/bin/sh", "-c", it.path});
    return true;
}

} // namespace waylaunch
