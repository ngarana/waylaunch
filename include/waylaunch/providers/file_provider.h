#pragma once

#include "waylaunch/providers/result_provider.h"
#include <string>
#include <vector>

namespace waylaunch {

class HistoryStore;

// Filename/foldername search via a native bounded filesystem walk, ranked by
// name match, path depth, recency and frecency. Async (runs on the search
// worker). Register only when [search].files is enabled.
class FileProvider : public ResultProvider {
public:
    FileProvider(std::vector<std::string> roots, std::vector<std::string> excludes,
                 int min_query, int max_results, const HistoryStore* history)
        : roots_(std::move(roots)), excludes_(std::move(excludes)),
          min_query_(min_query), max_results_(max_results), history_(history) {}

    std::string id() const override { return "files"; }
    bool is_async() const override { return true; }
    bool is_available() const override;
    std::vector<ListItem> query(const ProviderQuery&) override;
    bool activate(const ListItem&) override;

private:
    std::vector<std::string> roots_;
    std::vector<std::string> excludes_;
    int min_query_;
    int max_results_;
    const HistoryStore* history_;
};

} // namespace waylaunch
