#pragma once

// The indexing engine: consumes discovery (crawl) and change (inotify) events,
// decides what needs (re)indexing via mtime/size + content-hash (FR7), runs the
// format extractor, and writes to the Store. One background worker thread owns
// the read-write Store connection; heavy extraction is offloaded to subprocesses
// (the mdworker analog), so the thread is rarely CPU-bound and stays inside the
// resource envelope (NFR3/NFR4). Discovery/watcher threads only enqueue work.

#include "waylaunch/content/config.h"
#include "waylaunch/content/extractor.h"
#include "waylaunch/content/store.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace waylaunch::content {

class Indexer {
public:
    Indexer(Store& store, ContentConfig cfg);
    ~Indexer();

    void start();   // launch the worker (initial crawl + reconcile, then serve queue)
    void stop();    // signal and join
    void run_once(); // synchronous crawl + reconcile + maintain on the calling thread

    // Producers — safe to call from any thread (watcher, control).
    void enqueue_index(std::string path);
    void enqueue_remove(std::string path);
    void enqueue_remove_tree(std::string path);   // drop an entire subtree (dir gone)
    void request_reconcile();       // e.g. after inotify overflow / on demand
    void set_paused(bool paused);
    void request_reindex();         // drop the index and crawl from scratch
    void add_runtime_exclude(std::string path);  // exclude a path at runtime (FR8)

    // Path predicates (also used by the watcher to filter events).
    bool is_excluded(const std::string& path) const;   // dir noise + privacy
    bool under_roots(const std::string& path) const;

    struct Snapshot {
        int64_t indexed = 0;   // successful (re)index operations this run
        int64_t errors = 0;    // extractor failures marked state=error
        int64_t queued = 0;    // pending work items
        bool    crawling = false;
        bool    paused = false;
    };
    Snapshot snapshot() const;
    const ContentConfig& config() const { return cfg_; }

private:
    struct WorkItem { enum Kind { Index, Remove, RemoveTree } kind; std::string path; };

    void run_loop();
    void crawl_all();
    void reconcile_deletions();
    void index_file(const std::string& path);
    void wait_if_paused();

    Store&        store_;
    ContentConfig cfg_;
    Extractor     extractor_;
    size_t        max_file_bytes_;

    std::deque<WorkItem>     queue_;
    mutable std::mutex       mtx_;
    std::condition_variable  cv_;
    std::thread              thread_;

    mutable std::mutex       excl_mtx_;   // guards runtime_excludes_
    std::vector<std::string> runtime_excludes_;

    std::atomic<bool>    stop_{false};
    std::atomic<bool>    paused_{false};
    std::atomic<bool>    reconcile_{false};
    std::atomic<bool>    reindex_{false};
    std::atomic<bool>    crawling_{false};
    std::atomic<int64_t> indexed_{0};
    std::atomic<int64_t> errors_{0};
};

} // namespace waylaunch::content
