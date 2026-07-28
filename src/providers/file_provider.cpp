#include "waylaunch/providers/file_provider.h"
#include "waylaunch/history.h"
#include "waylaunch/subprocess.h"
#include "waylaunch/clipboard.h"
#include "waylaunch/search_util.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <queue>
#include <sys/stat.h>
#include <vector>

namespace waylaunch {

namespace {
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool matches_exclude(const std::string& name, const std::string& pat) {
    const bool has_glob = pat.find_first_of("*?[") != std::string::npos;
    if (!has_glob) return name == pat;

    const auto m = static_cast<long>(name.size());
    const auto n = static_cast<long>(pat.size());
    std::vector<std::vector<bool>> dp(n + 1, std::vector<bool>(m + 1, false));
    dp[0][0] = true;
    for (long i = 1; i <= n && pat[i - 1] == '*'; ++i) dp[i][0] = true;
    for (long i = 1; i <= n; ++i) {
        for (long j = 1; j <= m; ++j) {
            const char pc = pat[i - 1];
            if (pc == '*') dp[i][j] = dp[i - 1][j] || dp[i][j - 1];
            else if (pc == '?') dp[i][j] = dp[i - 1][j - 1];
            else dp[i][j] = dp[i - 1][j - 1] && pc == name[j - 1];
        }
    }
    return dp[n][m];
}

bool name_excluded(const std::string& name, const std::vector<std::string>& excludes) {
    for (const auto& ex : excludes) {
        if (matches_exclude(name, ex)) return true;
    }
    return false;
}
} // namespace

bool FileProvider::is_available() const {
    return true;
}

std::vector<ListItem> FileProvider::query(const ProviderQuery& q) {
    std::vector<ListItem> out;
    if (static_cast<int>(q.text.size()) < min_query_) return out;

    constexpr std::size_t kMaxVisited = 200000;
    constexpr int kDeadlineMs = 300;
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(kDeadlineMs);

    const std::string& ql = q.lower;
    std::size_t visited = 0;
    std::vector<ListItem> hits;

    for (const auto& root : roots_) {
        std::error_code ec;
        auto root_status = std::filesystem::status(root, ec);
        if (ec || !std::filesystem::is_directory(root_status)) continue;

        std::queue<std::string> dirs;
        dirs.push(root);

        while (!dirs.empty()) {
            if (++visited > kMaxVisited) goto done;
            if ((visited & 1023) == 0 && std::chrono::steady_clock::now() > deadline) goto done;

            std::string dir = std::move(dirs.front());
            dirs.pop();

            std::filesystem::directory_iterator it(dir, ec), end;
            if (ec) continue;
            for (; it != end; ++it) {
                const auto& entry = *it;
                std::string name = entry.path().filename().string();
                if (name == "." || name == "..") continue;
                if (name_excluded(name, excludes_)) continue;

                std::string path = entry.path().string();
                bool is_dir = entry.is_directory(ec);
                if (ec) {
                    struct stat st;
                    is_dir = (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
                    if (!is_dir && !S_ISREG(st.st_mode)) continue;
                }

                std::string nl = to_lower(name);
                if (nl.find(ql) == std::string::npos) {
                    if (is_dir) dirs.push(path);
                    continue;
                }

                struct stat st{};
                bool have_stat = (stat(path.c_str(), &st) == 0);
                if (have_stat && is_dir != S_ISDIR(st.st_mode)) is_dir = S_ISDIR(st.st_mode);

                ListItem it_item;
                it_item.kind = is_dir ? ItemKind::Folder : ItemKind::File;
                it_item.name = name;
                it_item.path = path;
                it_item.description = abbreviate_home(path);
                it_item.icon_name = is_dir ? "folder" : icon_for_file(path);

                size_t pos = nl.find(ql);
                float s = (pos == 0) ? 1000.0f - std::min<size_t>(nl.size(), 300)
                                      : 600.0f - std::min<size_t>(pos, 300);
                s -= path_depth(path) * 6.0f;
                if (is_dir) s += 15.0f;
                if (have_stat) s += recency_bonus(st.st_mtime);
                if (history_) s += history_->frecency(path);
                it_item.score = s;
                hits.push_back(std::move(it_item));

                if (is_dir) dirs.push(path);
            }
        }
    }

done:
    std::stable_sort(hits.begin(), hits.end(),
                     [](const ListItem& a, const ListItem& b) { return a.score > b.score; });
    if (hits.size() > static_cast<size_t>(std::max(1, max_results_)))
        hits.resize(static_cast<size_t>(std::max(1, max_results_)));
    out = std::move(hits);
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
