#pragma once

// The FSEvents analog: watch configured roots with inotify and report
// create/modify/delete/rename as index/remove work. Handles the Linux realities
// — dynamic watch add/remove as directories come and go, rename cookie pairing,
// and IN_Q_OVERFLOW (→ ask for a reconcile scan). Runs on its own thread and
// only calls back into the indexer's thread-safe enqueue methods.

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace waylaunch::content {

class FsWatcher {
public:
    using PathFn = std::function<void(const std::string&)>;
    using VoidFn = std::function<void()>;

    struct Callbacks {
        PathFn on_index;       // a file was created/modified/moved-in → (re)index it
        PathFn on_remove;      // a file was deleted/moved-out → drop it
        VoidFn on_overflow;    // event queue overflowed → reconcile the tree
        // A directory was deleted or moved out of the watched tree → drop every
        // indexed path under it. A moved-out directory produces no per-file
        // events, so without this its files stay in the index until the next
        // full reconcile (challenge §6.1).
        PathFn on_remove_tree;
        // inotify watch descriptors were exhausted (ENOSPC): some subtrees can't
        // be watched, so freshness there now depends on the periodic reconcile.
        // Fired once, the first time the limit is hit.
        VoidFn on_watch_limit;
    };

    // `excluded(path)` returns true for paths that must not be watched or reported.
    FsWatcher(std::vector<std::string> roots,
              std::function<bool(const std::string&)> excluded);
    ~FsWatcher();

    bool start(Callbacks cb);
    void stop();

    size_t watch_count() const;
    bool watch_limit_hit() const { return limit_hit_.load(); }

private:
    void thread_main();
    void drain_events();
    int  add_watch(const std::string& dir);          // one directory
    void add_watches(const std::string& dir);        // dir + existing subdirs
    void scan_subtree(const std::string& dir);       // add watches + enqueue files
    void drop_watch_subtree(const std::string& dir); // dir moved/deleted
    void remove_tree(const std::string& dir);        // enqueue subtree removal

    std::vector<std::string>                    roots_;
    std::function<bool(const std::string&)>     excluded_;
    Callbacks                                   cb_;

    int fd_ = -1;
    int stop_fd_ = -1;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> limit_hit_{false};

    mutable std::mutex maps_mtx_;
    std::unordered_map<int, std::string> wd_to_path_;
    std::unordered_map<std::string, int> path_to_wd_;
};

} // namespace waylaunch::content
