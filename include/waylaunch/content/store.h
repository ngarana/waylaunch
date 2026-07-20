#pragma once

// Content-search index store — a correctness-first wrapper over SQLite FTS5.
// This is the "inverted index" of the design (docs/CONTENT_SEARCH.md §4.3):
//
//   files    — metadata (path/size/mtime/hash/state); no text.
//   docs     — extracted text, zstd-compressed (body_z). Kept out of `files` so
//              metadata scans never drag body pages through the cache, and so
//              stored text costs ~30% of its raw size instead of 100% (NFR5).
//   docs_v   — view decompressing docs via wl_unzstd(); serves as the FTS5
//              external-content table, which is what lets snippet()/highlight()
//              read original text without a second uncompressed copy.
//   files_fts— FTS5 index over (name, body) with the custom "wl" tokenizer:
//              unicode61 plus CamelCase/digit segmentation, so OneTargetTwo /
//              one_target_two / one-target-two all match each other's terms.
//
// The same class serves both sides of the split: the daemon opens it read-write
// to index; the launcher opens it read-only to search. The reader prefers a
// strictly read-only handle (SQLite ≥3.22 reads WAL databases without the -shm
// by building a private wal-index) and reports *why* an index is unusable via
// availability(), so the caller can tell "no matches" from "no index" (NFR8).

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

// Why the index is or isn't usable. Distinguishing these matters for the UI:
// NoIndex/VersionMismatch → degrade to filename search; Ok + zero hits → simply
// show no content results (challenge §5.2).
enum class Availability {
    Ok,              // open and answering queries
    NoIndex,         // no database / no schema yet (daemon never ran)
    VersionMismatch, // built by an incompatible schema/tokenizer version
    Corrupt,         // failed integrity check (writer rebuilds; reader degrades)
    Locked,          // transiently locked/busy
    Error,           // any other hard failure
};

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
    bool      read_only = false;             // launcher opens read-only
    MatchMode match = MatchMode::Prefix;     // writer picks; reader adopts stored mode

    // Worst-case query governance (challenge §8). BM25 top-K over an ultra-common
    // term must score its whole posting list, so p99 grows with the corpus. The
    // planner consults fts5vocab document frequencies: when every query term is
    // more frequent than common_term_df, ranking degrades to "best of the
    // candidate_budget most recently indexed matches" — bounded work with sane
    // relevance. query_budget_ms is the backstop: a full ranked query that blows
    // the budget is interrupted and re-run bounded. 0 disables either mechanism.
    int64_t common_term_df  = 20000;
    int     candidate_budget = 2048;
    int     query_budget_ms  = 50;
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
    // tokenizer + zstd SQL functions, applies schema (writer), self-heals a
    // corrupt or version-mismatched DB by rebuilding (the index is derived
    // data). Returns false if unusable — availability() then says why, so a
    // reader can degrade to filename-only search (NFR8).
    bool open(const std::string& db_path, const StoreOptions& opts);
    void close();
    bool is_open() const { return db_ != nullptr; }
    Availability availability() const { return avail_; }

    // The match mode actually in effect (a reader adopts whatever the writer built).
    MatchMode match_mode() const { return match_; }
    const std::string& db_path() const { return path_; }

    // ---- writer (daemon) -------------------------------------------------
    // Batch many writes in one transaction (crawl speed). Nestable-safe: begin()
    // is a no-op if already in a batch; commit() ends it.
    bool begin();
    bool commit();
    // Upsert one file's metadata + extracted body (compressed into docs) in a
    // single savepoint; the docs triggers keep files_fts in sync.
    bool put(const FileRecord& rec, const std::string& body);
    // Update only mtime/size/indexed_at for a file whose content is unchanged
    // (FR7): never touches docs, so the FTS index is not rewritten.
    bool touch(const std::string& path, int64_t mtime_ns, int64_t size);
    // Hard-delete a path (removes its row, doc, and FTS entry immediately).
    bool remove(const std::string& path);
    // Hard-delete everything at or under `path` — a directory that was moved
    // out of the watched tree or deleted while events were lost (challenge §6).
    bool remove_subtree(const std::string& path);
    // Soft-delete: mark state=Tombstone and drop the doc/FTS entry now (so
    // queries need no state filter); the row is purged by maintain().
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
    // Rank content matches for a user query. Two-phase: rank rowids first, then
    // fetch metadata + snippet() for only the top K (snippets decompress the
    // body, so they must never be computed per candidate). Never throws; returns
    // {} on any error and downgrades availability() so the caller can tell
    // failure from "no matches".
    std::vector<ContentHit> search(const std::string& query, int limit,
                                   const std::string& hl_open = "",
                                   const std::string& hl_close = "");
    StoreStats stats();

private:
    bool open_handle(int flags);          // open + register tokenizer/functions
    bool recreate_db();                   // delete the files and start clean
    bool set_pragmas();
    bool apply_schema();
    bool ensure_match_mode(MatchMode requested);  // rebuild if the stored mode differs
    // Compile free text to an FTS5 MATCH expression. prefix_last completes the
    // final term with '*' (as-you-type); the bounded planner passes false so it
    // scans exact terms, which get FTS5's fast descending doclist walk.
    std::string build_match_query(const std::string& user_query, bool prefix_last) const;
    bool query_is_common(const std::string& user_query) const;
    bool ranked_rowids_full(const std::string& match, int limit,
                            std::vector<std::pair<int64_t, double>>& out);
    void ranked_rowids_bounded(const std::string& match, int limit,
                               std::vector<std::pair<int64_t, double>>& out);

    sqlite3*     db_ = nullptr;
    std::string  path_;
    MatchMode    match_ = MatchMode::Prefix;
    bool         read_only_ = false;
    Availability avail_ = Availability::NoIndex;
    StoreOptions opts_;
};

} // namespace waylaunch::content
