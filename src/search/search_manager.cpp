#include "waylaunch/search_manager.h"
#include "waylaunch/subprocess.h"
#include <algorithm>
#include <sstream>
#include <thread>
#include <mutex>
#include <nlohmann/json.hpp>
#include <filesystem>

namespace waylaunch {

// FdFzfBackend
std::vector<SearchResult> FdFzfBackend::search(const SearchParams& params) {
    std::vector<SearchResult> results;

    std::vector<std::string> fd_args = {"fd", "--format", "{}"};
    switch (params.mode) {
        case SearchMode::Files: fd_args.insert(fd_args.end(), {"--type", "file"}); break;
        case SearchMode::Directories: fd_args.insert(fd_args.end(), {"--type", "directory"}); break;
        case SearchMode::Applications: fd_args.insert(fd_args.end(), {"--type", "file", "--extension", "desktop"}); break;
        default: fd_args.insert(fd_args.end(), {"--type", "file"}); break;
    }

    if (params.include_hidden) fd_args.push_back("-H");
    if (params.include_ignored) fd_args.push_back("-I");
    if (!params.query.empty()) fd_args.push_back(params.query);
    if (!params.search_path.empty()) fd_args.push_back(params.search_path);

    auto fd_result = Subprocess::run(fd_args);
    if (fd_result.exit_code != 0 || fd_result.stdout.empty()) return results;

    if (params.query.empty()) {
        std::istringstream stream(fd_result.stdout);
        std::string line;
        int count = 0;
        while (std::getline(stream, line) && count < params.max_results) {
            if (!line.empty()) {
                SearchResult r;
                r.path = line;
                r.display_name = std::filesystem::path(line).filename().string();
                r.display_path = line;
                r.score = 1.0f;
                results.push_back(std::move(r));
                count++;
            }
        }
        return results;
    }

    std::vector<std::string> fzf_args = {"fzf", "--filter", params.query, "--print-query", "--no-sort"};
    auto fzf_result = Subprocess::run(fzf_args, fd_result.stdout);

    std::istringstream stream(fzf_result.stdout);
    std::string line;
    bool first_line = true;
    while (std::getline(stream, line) && (int)results.size() < params.max_results) {
        if (first_line) { first_line = false; continue; }
        if (!line.empty()) {
            SearchResult r;
            r.path = line;
            r.display_name = std::filesystem::path(line).filename().string();
            r.display_path = line;
            r.score = 1.0f;
            results.push_back(std::move(r));
        }
    }

    return results;
}

bool FdFzfBackend::is_available() const {
    return Subprocess::command_exists("fd") && Subprocess::command_exists("fzf");
}

// RipgrepBackend
std::vector<SearchResult> RipgrepBackend::search(const SearchParams& params) {
    std::vector<SearchResult> results;

    std::vector<std::string> rg_args = {"rg", "--json", "--no-heading", "-n", "--context", "0", "--max-count", "1"};
    if (!params.include_hidden) rg_args.insert(rg_args.end(), {"--glob", "!.*/"});
    rg_args.insert(rg_args.end(), {"-e", params.query});
    if (!params.search_path.empty()) rg_args.push_back(params.search_path);

    auto rg_result = Subprocess::run(rg_args);
    if (rg_result.exit_code != 0 && rg_result.exit_code != 1) return results;

    std::istringstream stream(rg_result.stdout);
    std::string line;
    while (std::getline(stream, line) && (int)results.size() < params.max_results) {
        if (line.empty()) continue;
        try {
            auto j = nlohmann::json::parse(line, nullptr, false);
            if (j.is_discarded() || j["type"] != "match") continue;

            SearchResult r;
            r.path = j["data"]["path"]["text"];
            r.display_name = std::filesystem::path(r.path).filename().string();
            r.display_path = r.path;
            r.line_number = j["data"]["line_number"];
            r.snippet = j["data"]["lines"]["text"];
            r.score = 1.0f;

            if (j["data"].contains("submatches")) {
                for (auto& sub : j["data"]["submatches"]) {
                    r.highlight_ranges.push_back({sub["start"], sub["end"]});
                }
            }
            results.push_back(std::move(r));
        } catch (...) { continue; }
    }

    return results;
}

bool RipgrepBackend::is_available() const {
    return Subprocess::command_exists("rg");
}

// FuzzyMatcher
FuzzyMatcher::FuzzyMatcher(const std::string& pattern) : pattern_(pattern) {
    pattern_lower_.resize(pattern.size());
    for (size_t i = 0; i < pattern.size(); i++) {
        pattern_lower_[i] = std::tolower(pattern[i]);
    }
}

FuzzyMatcher::MatchResult FuzzyMatcher::match(const std::string& candidate) const {
    MatchResult result;
    if (pattern_.empty()) { result.matched = true; result.score = 1.0f; return result; }
    if (candidate.empty()) return result;

    std::string cl(candidate);
    std::transform(cl.begin(), cl.end(), cl.begin(), ::tolower);

    size_t ci = 0;
    for (size_t pi = 0; pi < pattern_.size(); pi++) {
        bool found = false;
        while (ci < candidate.size()) {
            if (cl[ci] == static_cast<char>(pattern_lower_[pi])) {
                found = true;
                result.positions.push_back(ci);
                ci++;
                break;
            }
            ci++;
        }
        if (!found) return result;
    }

    float score = 0.0f;
    int consecutive = 0;
    for (size_t i = 1; i < result.positions.size(); i++) {
        if (result.positions[i] == result.positions[i-1] + 1) consecutive++;
    }
    score += consecutive * 2.0f;
    if (result.positions[0] == 0) score += 3.0f;
    for (int pos : result.positions) {
        if (pos == 0 || candidate[pos-1] == '/' || candidate[pos-1] == '_' || candidate[pos-1] == '-' || candidate[pos-1] == '.') {
            score += 1.0f;
        }
    }
    score -= candidate.size() * 0.01f;

    result.matched = true;
    result.score = std::max(0.1f, score);
    return result;
}

// SearchManager
SearchManager::SearchManager() : fd_fzf_backend_(std::make_unique<FdFzfBackend>()),
                                 rg_backend_(std::make_unique<RipgrepBackend>()) {}

SearchManager::~SearchManager() { cancel(); }

void SearchManager::set_search_path(const std::string& path) { search_path_ = path; }
void SearchManager::set_max_results(int max) { max_results_ = max; }
void SearchManager::set_debounce_ms(int ms) { debounce_ms_ = ms; }

void SearchManager::search(const std::string& query, SearchMode mode, SearchCallback callback) {
    cancel();
    std::lock_guard lock(mutex_);
    current_search_id_++;
    int search_id = current_search_id_;
    cancel_requested_ = false;

    search_thread_ = std::thread([this, query, mode, callback, search_id]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(debounce_ms_));
        {
            std::lock_guard l(mutex_);
            if (cancel_requested_ || current_search_id_ != search_id) return;
        }
        do_search(query, mode, callback);
    });
}

void SearchManager::cancel() {
    {
        std::lock_guard lock(mutex_);
        cancel_requested_ = true;
        current_search_id_++;
    }
    if (search_thread_.joinable()) search_thread_.join();
}

void SearchManager::do_search(const std::string& query, SearchMode mode, SearchCallback callback) {
    SearchParams params;
    params.query = query;
    params.search_path = search_path_;
    params.mode = mode;
    params.max_results = max_results_;

    std::vector<SearchResult> results;
    switch (mode) {
        case SearchMode::Files:
        case SearchMode::Directories:
            if (fd_fzf_backend_->is_available()) results = fd_fzf_backend_->search(params);
            break;
        case SearchMode::FileContents:
            if (rg_backend_->is_available()) results = rg_backend_->search(params);
            break;
        default: break;
    }
    callback(results);
}

bool SearchManager::has_fd() const { return fd_fzf_backend_->is_available(); }
bool SearchManager::has_rg() const { return rg_backend_->is_available(); }
bool SearchManager::has_fzf() const { return Subprocess::command_exists("fzf"); }

} // namespace waylaunch
