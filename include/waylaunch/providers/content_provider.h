#pragma once

#include "waylaunch/providers/result_provider.h"

namespace waylaunch {

namespace content {
class Store;
}
class HistoryStore;

// Full-text CONTENTS search over the waylaunchd index (BM25-ranked, best-first).
// Async (runs on the search worker). Register only when the index is open.
class ContentProvider : public ResultProvider {
  public:
    ContentProvider(content::Store* store, int min_query, int max_results,
                    const HistoryStore* history)
        : store_(store), min_query_(min_query), max_results_(max_results), history_(history) {}

    std::string id() const override { return "content"; }
    bool is_async() const override { return true; }
    bool is_available() const override { return store_ != nullptr; }
    std::vector<ListItem> query(const ProviderQuery&) override;
    bool activate(const ListItem&) override;

  private:
    content::Store* store_;
    int min_query_;
    int max_results_;
    const HistoryStore* history_;
};

} // namespace waylaunch
