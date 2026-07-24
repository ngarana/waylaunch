#pragma once

#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace waylaunch {

struct HistoryEntry {
    std::string query;
    std::string key;
    std::string label;
    std::time_t last_used = 0;
    uint32_t uses = 0;
};

// Small line-oriented persistent store for query history and activation
// frecency. It is deliberately independent of Wayland/UI code so it can be
// tested in a host-only target and safely queried by the file-search worker.
class HistoryStore {
public:
    void configure(std::string path, int max_entries, int max_age_days,
                   double half_life_days);
    bool load();
    void record(const std::string& query, const std::string& key,
                const std::string& label, std::time_t now = std::time(nullptr));

    std::vector<HistoryEntry> recent(std::size_t limit) const;
    float frecency(const std::string& key,
                   std::time_t now = std::time(nullptr)) const;

    static std::string default_path();

private:
    mutable std::mutex mutex_;
    std::string path_;
    int max_entries_ = 100;
    int max_age_days_ = 365;
    double half_life_days_ = 30.0;
    std::vector<HistoryEntry> entries_;

    void prune_locked(std::time_t now);
    bool save_locked() const;
};

} // namespace waylaunch
