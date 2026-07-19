#pragma once

// Configuration for content search, parsed from the `[content]` table of the
// shared config.toml (docs/CONTENT_SEARCH.md §10). The daemon uses every field;
// the launcher reads only enable/min_query/max_results/match. Roots and excludes
// fall back to the existing `[search]` file-search settings when unset, so a user
// who configured file search gets content search over the same tree for free.

#include "waylaunch/content/store.h"       // MatchMode
#include "waylaunch/content/extractor.h"   // ExtractOptions

#include <string>
#include <vector>

namespace waylaunch::content {

struct ContentConfig {
    bool                     enable = true;         // master switch (also gates UI)
    std::vector<std::string> roots;                 // absolute, ~-expanded
    std::vector<std::string> excludes;              // dir names / path substrings skipped
    std::vector<std::string> exclude_paths;         // absolute privacy prefixes never read
    size_t                   max_file_mb = 8;        // don't open files larger than this
    size_t                   max_text_mb = 2;        // cap extracted text per file
    MatchMode                match = MatchMode::Prefix;  // "prefix" | "substring"
    int                      min_query = 3;          // chars before content search fires
    int                      max_results = 6;
    std::vector<std::string> extractors = {"text", "pdf", "office", "html"};
    bool                     throttle_on_battery = true;
    int                      worker_nice = 10;
    size_t                   max_index_mb = 0;        // 0 = unlimited hard cap

    // Effective extractor options derived from the caps above.
    ExtractOptions extract_options() const;

    // Standard per-user locations.
    static std::string db_path();         // $XDG_DATA_HOME/waylaunch/index.db
    static std::string socket_path();     // $XDG_RUNTIME_DIR/waylaunch/waylaunchd.sock
    static std::string runtime_dir();     // $XDG_RUNTIME_DIR/waylaunch
};

// Parse [content] (+ [search] fallbacks) from a config.toml. Missing file →
// defaults. `config_path` empty → the launcher's default config path.
ContentConfig load_content_config(const std::string& config_path = "");

// Expand a leading "~" to $HOME. Absolute/other paths returned unchanged.
std::string expand_tilde(const std::string& p);

} // namespace waylaunch::content
