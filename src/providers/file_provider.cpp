#include "waylaunch/providers/file_provider.h"
#include "waylaunch/history.h"
#include "waylaunch/subprocess.h"
#include "waylaunch/search_util.h"
#include "waylaunch/clipboard.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <sys/stat.h>

namespace waylaunch {

namespace {
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}
} // namespace

bool FileProvider::is_available() const {
    return Subprocess::command_exists("fd");
}

std::vector<ListItem> FileProvider::query(const ProviderQuery& q) {
    std::vector<ListItem> out;
    // Availability (fd present) is checked by the caller before query(); here we
    // only gate on the minimum query length.
    if (static_cast<int>(q.text.size()) < min_query_) return out;

    // Match against the file *name*; scan a bounded number of hits, then rank and
    // keep the best few. Noise directories are excluded.
    std::vector<std::string> argv = {
        "fd", "--color", "never", "--fixed-strings", "--max-results", "200",
        "--type", "f", "--type", "d"};
    for (const auto& ex : excludes_) {
        argv.push_back("--exclude");
        argv.push_back(ex);
    }
    argv.push_back(q.text);
    for (const auto& r : roots_) argv.push_back(r);
    auto res = Subprocess::run(argv);

    const std::string ql = q.lower;
    std::istringstream ss(res.stdout);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        struct stat st;
        bool have_stat = (stat(line.c_str(), &st) == 0);
        bool is_dir = have_stat ? S_ISDIR(st.st_mode) : false;

        ListItem it;
        it.kind = is_dir ? ItemKind::Folder : ItemKind::File;
        std::filesystem::path p(line);
        it.name = p.filename().string();
        if (it.name.empty()) it.name = line;
        it.path = line;
        it.description = abbreviate_home(line);
        it.icon_name = is_dir ? "folder" : icon_for_file(line);

        // Rank: prefix > substring on the name; shallower paths and more recently
        // modified files score higher.
        std::string nl = to_lower(it.name);
        size_t pos = nl.find(ql);
        float s;
        if (pos == 0)                        s = 1000.0f - std::min<size_t>(nl.size(), 300);
        else if (pos != std::string::npos)   s = 600.0f - std::min<size_t>(pos, 300);
        else                                 s = 200.0f;   // matched deeper in the path
        s -= path_depth(line) * 6.0f;
        if (is_dir) s += 15.0f;
        if (have_stat) s += recency_bonus(st.st_mtime);
        if (history_) s += history_->frecency(line);
        it.score = s;

        out.push_back(std::move(it));
    }

    std::stable_sort(out.begin(), out.end(),
                     [](const ListItem& a, const ListItem& b) { return a.score > b.score; });
    if (out.size() > static_cast<size_t>(std::max(1, max_results_)))
        out.resize(std::max(1, max_results_));
    return out;
}

bool FileProvider::activate(const ListItem& it) {
    if (it.kind != ItemKind::File && it.kind != ItemKind::Folder) return false;
    if (!it.path.empty()) {
        Clipboard::copy_file_path(it.path);
        Subprocess::spawn_detached({"xdg-open", it.path});
    }
    return true;
}

} // namespace waylaunch
