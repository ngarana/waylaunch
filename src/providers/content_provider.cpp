#include "waylaunch/providers/content_provider.h"
#include "waylaunch/content/store.h"
#include "waylaunch/history.h"
#include "waylaunch/search_util.h"
#include "waylaunch/subprocess.h"
#include "waylaunch/clipboard.h"

#include <filesystem>
#include <string>

namespace waylaunch {

std::vector<ListItem> ContentProvider::query(const ProviderQuery& q) {
    std::vector<ListItem> out;
    if (!store_ || static_cast<int>(q.text.size()) < min_query_) return out;

    auto hits = store_->search(q.text, max_results_,
                               std::string(1, kHlOpen), std::string(1, kHlClose));
    for (auto& h : hits) {
        ListItem it;
        it.kind = ItemKind::Content;
        std::filesystem::path p(h.path);
        it.name = h.name.empty() ? p.filename().string() : h.name;
        it.path = h.path;
        it.reveal_path = h.path;
        it.description = abbreviate_home(h.path);
        it.snippet = h.snippet;
        it.icon_name = icon_for_file(h.path);
        it.score = static_cast<float>(h.score);
        if (history_) it.score += 0.25f * history_->frecency(h.path);
        out.push_back(std::move(it));
    }
    return out;
}

bool ContentProvider::activate(const ListItem& it) {
    if (it.kind != ItemKind::Content) return false;
    if (!it.path.empty()) {
        Clipboard::copy_file_path(it.path);
        Subprocess::spawn_detached({"xdg-open", it.path});
    }
    return true;
}

} // namespace waylaunch
