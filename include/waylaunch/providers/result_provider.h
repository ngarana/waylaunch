#pragma once

#include "waylaunch/list_item.h"
#include <string>
#include <vector>

namespace waylaunch {

// The query handed to a provider. `lower` is the lowercased `text`, precomputed
// once so providers don't each re-lowercase for case-insensitive matching.
struct ProviderQuery {
    std::string text;
    std::string lower;
    int max_results = 6;
};

// A pluggable search source. Adding a mode (emoji, web search, ...) becomes a new
// class + one registration line — the switch statements go away (OCP + SRP).
//
// query() runs on the UI thread for cheap providers, or on the file-search worker
// thread for async ones (is_async() == true). Either way it MUST NOT touch
// Wayland/Cairo — results are marshaled back and rendered on the UI thread.
class ResultProvider {
public:
    virtual ~ResultProvider() = default;

    virtual std::string id() const = 0;                // "calculator", "applications", ...
    virtual bool is_available() const { return true; } // e.g. index open
    virtual bool is_async() const { return false; }    // true → run on the worker thread

    virtual std::vector<ListItem> query(const ProviderQuery&) = 0;

    // Activate an item this provider produced. Return true if handled (the caller
    // then closes the launcher); false to let another handler try. Providers key
    // off the item's kind/fields to recognise their own results.
    virtual bool activate(const ListItem&) = 0;
};

} // namespace waylaunch
