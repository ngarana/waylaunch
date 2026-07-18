#include "waylaunch/finder_search.h"
#include "waylaunch/subprocess.h"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <thread>
#include <mutex>
#include <filesystem>
#include <dirent.h>
#include <sys/stat.h>

namespace waylaunch {

FinderSearch::FinderSearch() {
    scope_.type = SearchScopeType::ThisFolder;
    scope_.name = "This Folder";
    scope_.path = ".";
}

FinderSearch::~FinderSearch() {
    cancel();
}

void FinderSearch::set_current_directory(const std::string& path) {
    current_directory_ = path;
    if (scope_.type == SearchScopeType::ThisFolder) {
        scope_.path = path;
    }
}

const std::string& FinderSearch::current_directory() const {
    return current_directory_;
}

void FinderSearch::set_scope(const SearchScopeInfo& scope) {
    scope_ = scope;
    if (scope_.type == SearchScopeType::ThisFolder) {
        scope_.path = current_directory_;
    }
}

const SearchScopeInfo& FinderSearch::scope() const {
    return scope_;
}

void FinderSearch::set_max_results(int max) {
    max_results_ = max;
}

void FinderSearch::set_include_hidden(bool include) {
    include_hidden_ = include;
}

void FinderSearch::search(const std::string& query, FinderSearchCallback callback) {
    cancel();

    std::lock_guard lock(mutex_);
    current_search_id_++;
    int search_id = current_search_id_;
    cancel_requested_ = false;

    search_thread_ = std::thread([this, query, callback, search_id]() {
        {
            std::lock_guard l(mutex_);
            if (cancel_requested_ || current_search_id_ != search_id) return;
        }
        do_search(query, callback);
    });
}

void FinderSearch::cancel() {
    {
        std::lock_guard lock(mutex_);
        cancel_requested_ = true;
        current_search_id_++;
    }
    if (search_thread_.joinable()) search_thread_.join();
}

std::vector<SearchScopeInfo> FinderSearch::get_available_scopes() const {
    std::vector<SearchScopeInfo> scopes;

    scopes.push_back({SearchScopeType::ThisFolder, "This Folder", current_directory_});
    scopes.push_back({SearchScopeType::ThisMac, "This Mac", "/"});
    scopes.push_back({SearchScopeType::Recents, "Recents", ""});

    return scopes;
}

bool FinderSearch::has_fd() const {
    return Subprocess::command_exists("fd");
}

void FinderSearch::do_search(const std::string& query, FinderSearchCallback callback) {
    std::vector<FinderSearchResult> results;

    if (query.empty()) {
        callback(results, 0);
        return;
    }

    switch (scope_.type) {
        case SearchScopeType::ThisFolder:
            results = search_this_folder(query);
            break;
        case SearchScopeType::ThisMac:
            results = search_this_mac(query);
            break;
        case SearchScopeType::Recents:
            results = search_recents(query);
            break;
        default:
            results = search_this_folder(query);
            break;
    }

    callback(results, static_cast<int>(results.size()));
}

std::vector<FinderSearchResult> FinderSearch::search_this_folder(const std::string& query) {
    std::vector<FinderSearchResult> results;

    std::vector<std::string> fd_args = {"fd", "--format", "{}", "--max-depth", "5"};
    if (!include_hidden_) fd_args.push_back("--hidden");
    fd_args.push_back(query);
    fd_args.push_back(current_directory_);

    auto result = Subprocess::run(fd_args);
    if (result.exit_code != 0 || result.stdout.empty()) return results;

    std::istringstream stream(result.stdout);
    std::string line;
    while (std::getline(stream, line) && static_cast<int>(results.size()) < max_results_) {
        if (line.empty()) continue;

        FinderSearchResult r;
        r.path = line;
        r.name = std::filesystem::path(line).filename().string();
        r.extension = std::filesystem::path(line).extension().string();

        struct stat st;
        if (stat(line.c_str(), &st) == 0) {
            r.is_directory = S_ISDIR(st.st_mode);
            r.size = st.st_size;
            r.modified_time = st.st_mtime;
        }

        r.kind = FileModel::file_kind_from_path(line);
        r.score = 1.0f;
        results.push_back(std::move(r));
    }

    return results;
}

std::vector<FinderSearchResult> FinderSearch::search_this_mac(const std::string& query) {
    std::vector<FinderSearchResult> results;

    std::vector<std::string> fd_args = {"fd", "--format", "{}", "--max-depth", "4"};
    if (!include_hidden_) fd_args.push_back("--hidden");
    fd_args.push_back(query);

    auto result = Subprocess::run(fd_args);
    if (result.exit_code != 0 || result.stdout.empty()) return results;

    std::istringstream stream(result.stdout);
    std::string line;
    while (std::getline(stream, line) && static_cast<int>(results.size()) < max_results_) {
        if (line.empty()) continue;

        FinderSearchResult r;
        r.path = line;
        r.name = std::filesystem::path(line).filename().string();
        r.extension = std::filesystem::path(line).extension().string();

        struct stat st;
        if (stat(line.c_str(), &st) == 0) {
            r.is_directory = S_ISDIR(st.st_mode);
            r.size = st.st_size;
            r.modified_time = st.st_mtime;
        }

        r.kind = FileModel::file_kind_from_path(line);
        r.score = 1.0f;
        results.push_back(std::move(r));
    }

    return results;
}

std::vector<FinderSearchResult> FinderSearch::search_recents(const std::string& query) {
    std::vector<FinderSearchResult> results;

    const char* home = std::getenv("HOME");
    if (!home) return results;

    std::string recent_path = std::string(home) + "/.local/share/recently-used.xbel";
    std::ifstream file(recent_path);
    if (!file.is_open()) return results;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    size_t pos = 0;
    while ((pos = content.find("href=\"file://", pos)) != std::string::npos && static_cast<int>(results.size()) < max_results_) {
        pos += 13;
        size_t end = content.find("\"", pos);
        if (end != std::string::npos) {
            std::string path = content.substr(pos, end - pos);

            std::string name = std::filesystem::path(path).filename().string();
            std::string query_lower = query;
            std::string name_lower = name;
            std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);
            std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

            if (name_lower.find(query_lower) != std::string::npos) {
                FinderSearchResult r;
                r.path = path;
                r.name = name;
                r.extension = std::filesystem::path(path).extension().string();

                struct stat st;
                if (stat(path.c_str(), &st) == 0) {
                    r.is_directory = S_ISDIR(st.st_mode);
                    r.size = st.st_size;
                    r.modified_time = st.st_mtime;
                }

                r.kind = FileModel::file_kind_from_path(path);
                r.score = 1.0f;
                results.push_back(std::move(r));
            }
        }
    }

    return results;
}

} // namespace waylaunch
