#pragma once

// Content-search index store — a thin, correctness-first wrapper over SQLite
// FTS5. This is the "inverted index" of the design (docs/CONTENT_SEARCH.md §4.3):
// a `files` metadata table plus an external-content `files_fts` virtual table
// with BM25 ranking and a custom "wl" tokenizer (unicode61 + CamelCase/underscore/
// hyphen splitting) so OneTargetTwo / one_target_two / one-target-two all match.
//
// The same class serves both sides of the split: the daemon opens it read-write
// to index; the launcher opens it read-only (query_only) to search. Opening is
// crash-safe (integrity check + rebuild) because the index is derived data.

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <cstdint>

struct sqlite3;   // forward-declared; sqlite3.h is an implementation detail

namespace waylaunch::content {

// Lifecycle of a row (mirrors Spotlight's transient states).
enum class FileState : int { Indexed = 0, Pending = 1, Tombstone = 2, Error = 3 };

// Word/prefix ("as you type") vs arbitrary substring (trigram) matching.
enum class MatchMode { Prefix, Substring };

// One file as tracked in the index. `body` (extracted text) is passed to put()
// separately rather than living here, since it can be large.
struct FileRecord {
    int64_t     id = 0;
    std::string path;          // absolute
    std::string name;          // basename
    std::string parent;        // dirname
    int64_t     size = 0;
    int64_t     mtime_ns = 0;  // change detection
    std::string mime;
    std::string content_hash;  // raw bytes; skip re-extraction when unchanged (FR7)
    int64_t     indexed_at = 0;
    FileState   state = FileState::Pending;
};

// A single content-search hit returned to the launcher.
struct ContentHit {
    std::string path;
    std::string name;
    std::string mime;
    std::string snippet;   // FTS5 snippet() of the matched body window
    double      score = 0.0;   // normalized so higher == better
};

struct StoreStats {
    int64_t total = 0;
    int64_t indexed = 0;
    int64_t pending = 0;
    int64_t errored = 0;
    int64_t tombstoned = 0;
    int64_t db_bytes = 0;
};

struct StoreOptions {
    bool      read_only = false;             // launcher opens read-only (query_only)
    MatchMode match = MatchMode::Prefix;     // writer picks; reader adopts stored mode
};

class Store {
public:
    Store() = default;
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&&) noexcept;
    Store& operator=(Store&&) noexcept;

    // Open (creating when writable) the index at db_path. Registers the custom
    // tokenizer, applies schema + migrations (writer), and self-heals a corrupt
    // DB by rebuilding. Returns false if the index is unusable (reader → degrade
    // to filename-only search; NFR8).
    bool open(const std::string& db_path, const StoreOptions& opts);
    void close();
    bool is_open() const { return db_ != nullptr; }

    // The match mode actually in effect (a reader adopts whatever the writer built).
    MatchMode match_mode() const { return match_; }
    const std::string& db_path() const { return path_; }

    // ---- writer (daemon) -------------------------------------------------
    // Batch many writes in one transaction (crawl speed). Nestable-safe: begin()
    // is a no-op if already in a batch; commit() ends it.
    bool begin();
    bool commit();
    // Upsert one file's metadata + extracted body in a single transaction.
    bool put(const FileRecord& rec, const std::string& body);
    // Update only mtime/size/indexed_at for a file whose content is unchanged
    // (FR7): does not touch the FTS index (trigger fires only on name/body).
    bool touch(const std::string& path, int64_t mtime_ns, int64_t size);
    // Hard-delete a path (removes its row and FTS entry immediately).
    bool remove(const std::string& path);
    // Soft-delete: mark state=Tombstone (filtered from queries; purged by maintain()).
    bool tombstone(const std::string& path);
    // Fetch the stored record for change detection. nullopt if absent.
    std::optional<FileRecord> get(const std::string& path);
    // Visit every live (non-tombstone) path — used by the startup deletion
    // reconcile to find rows whose file no longer exists.
    bool for_each_indexed_path(const std::function<void(const std::string&)>& fn);
    // Drop all rows and rebuild the (now empty) FTS index — full reindex.
    bool clear();
    // Purge tombstones and merge FTS segments (transient→static housekeeping).
    bool maintain();

    // ---- reader (launcher + daemon) --------------------------------------
    // Rank content matches for a user query. Never throws; returns {} on any
    // error (malformed query, locked/absent DB) so the caller degrades cleanly.
    std::vector<ContentHit> search(const std::string& query, int limit,
                                   const std::string& hl_open = "",
                                   const std::string& hl_close = "");
    StoreStats stats();

private:
    bool set_pragmas(const StoreOptions& opts);
    bool self_check_or_rebuild();
    bool apply_schema();
    bool ensure_match_mode(MatchMode requested);  // rebuild if the stored mode differs
    std::string build_match_query(const std::string& user_query) const;

    sqlite3*    db_ = nullptr;
    std::string path_;
    MatchMode   match_ = MatchMode::Prefix;
    bool        read_only_ = false;
};

} // namespace waylaunch::content
