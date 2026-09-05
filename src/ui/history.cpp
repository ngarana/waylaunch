#include "waylaunch/history.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace waylaunch {
namespace {

std::string escape_field(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\\') out += "\\\\";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

std::vector<std::string> split_fields(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    bool escaped = false;
    for (char c : line) {
        if (escaped) {
            if (c == 't') current += '\t';
            else if (c == 'n') current += '\n';
            else current += c;
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '\t') {
            fields.push_back(std::move(current));
            current.clear();
        } else {
            current += c;
        }
    }
    if (escaped) current += '\\';
    fields.push_back(std::move(current));
    return fields;
}

} // namespace

void HistoryStore::configure(std::string path, int max_entries, int max_age_days,
                             double half_life_days) {
    std::scoped_lock lock(mutex_);
    path_ = std::move(path);
    max_entries_ = std::max(1, max_entries);
    max_age_days_ = std::max(1, max_age_days);
    half_life_days_ = std::max(1.0, half_life_days);
}

bool HistoryStore::load() {
    std::scoped_lock lock(mutex_);
    entries_.clear();
    if (path_.empty()) return false;

    std::ifstream file(path_);
    if (!file) return true; // First run: an absent history file is normal.

    std::string line;
    while (std::getline(file, line)) {
        auto fields = split_fields(line);
        if (fields.size() != 5 || fields[0] != "1") continue;
        try {
            HistoryEntry entry;
            entry.last_used = static_cast<std::time_t>(std::stoll(fields[1]));
            entry.uses = static_cast<uint32_t>(std::stoul(fields[2]));
            entry.query = std::move(fields[3]);
            entry.key = std::move(fields[4]);
            entry.label = entry.key;
            if (!entry.query.empty() && !entry.key.empty() && entry.uses > 0)
                entries_.push_back(std::move(entry));
        } catch (const std::exception&) {
            // Ignore one malformed history row; the remaining history remains usable.
            (void) 0;
        }
    }
    prune_locked(std::time(nullptr));
    return true;
}

void HistoryStore::record(const std::string& query, const std::string& key,
                          const std::string& label, std::time_t now) {
    if (query.empty() || key.empty()) return;
    std::scoped_lock lock(mutex_);
    auto it = std::ranges::find_if(entries_, [&](const HistoryEntry& entry) {
        return entry.query == query && entry.key == key;
    });
    if (it == entries_.end()) {
        entries_.push_back(
            {.query = query, .key = key, .label = label, .last_used = now, .uses = 1});
    } else {
        it->last_used = now;
        it->uses = std::min<uint32_t>(it->uses + 1, 1000000U);
        if (!label.empty()) it->label = label;
    }
    prune_locked(now);
    save_locked();
}

std::vector<HistoryEntry> HistoryStore::recent(std::size_t limit) const {
    std::scoped_lock lock(mutex_);
    // Keep only the most recent activation for each query in the suggestion
    // list; the underlying per-target rows remain available for frecency.
    std::map<std::string, HistoryEntry> by_query;
    for (const auto& entry : entries_) {
        auto it = by_query.find(entry.query);
        if (it == by_query.end() || entry.last_used > it->second.last_used)
            by_query[entry.query] = entry;
    }
    std::vector<HistoryEntry> result;
    result.reserve(by_query.size());
    for (auto& [_, entry] : by_query) result.push_back(std::move(entry));
    std::ranges::stable_sort(result, [](const auto& a, const auto& b) {
        if (a.last_used != b.last_used) return a.last_used > b.last_used;
        return a.uses > b.uses;
    });
    if (result.size() > limit) result.resize(limit);
    return result;
}

float HistoryStore::frecency(const std::string& key, std::time_t now) const {
    if (key.empty()) return 0.0F;
    std::scoped_lock lock(mutex_);
    double score = 0.0;
    for (const auto& entry : entries_) {
        if (entry.key != key) continue;
        double age_days = std::max(0.0, std::difftime(now, entry.last_used) / 86400.0);
        score += 220.0 * std::sqrt(static_cast<double>(entry.uses)) *
                 std::exp(-age_days / half_life_days_);
    }
    return static_cast<float>(std::min(score, 900.0));
}

std::string HistoryStore::default_path() {
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state)
        return (std::filesystem::path(state) / "waylaunch/history.tsv").string();
    if (const char* home = std::getenv("HOME"); home && *home)
        return (std::filesystem::path(home) / ".local/state/waylaunch/history.tsv").string();
    return ".waylaunch-history.tsv";
}

void HistoryStore::prune_locked(std::time_t now) {
    const auto oldest = now - (static_cast<std::time_t>(max_age_days_) * 86400);
    auto removed = std::ranges::remove_if(
        entries_, [&](const auto& entry) { return entry.last_used < oldest; });
    entries_.erase(removed.begin(), removed.end());
    if (entries_.size() <= static_cast<std::size_t>(max_entries_)) return;
    std::ranges::stable_sort(
        entries_, [](const auto& a, const auto& b) { return a.last_used > b.last_used; });
    entries_.resize(static_cast<std::size_t>(max_entries_));
}

bool HistoryStore::save_locked() const {
    if (path_.empty()) return false;
    std::error_code error;
    const std::filesystem::path path(path_);
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return false;
    }
    const auto temporary = path.string() + ".tmp";
    std::ofstream file(temporary, std::ios::trunc);
    if (!file) return false;
    for (const auto& entry : entries_) {
        file << "1\t" << entry.last_used << '\t' << entry.uses << '\t' << escape_field(entry.query)
             << '\t' << escape_field(entry.key) << '\n';
    }
    file.close();
    if (!file) return false;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    return !error;
}

} // namespace waylaunch
