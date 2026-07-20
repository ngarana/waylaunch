// Freshness backstop (§4.5/§9, NFR2): the periodic reconcile must catch
// create/modify/delete that inotify never reported — the situation when watch
// descriptors are exhausted and a subtree is unwatched. We prove it by running
// the Indexer with a short reconcile interval and NO FsWatcher at all, so the
// only thing that can update the index is the periodic pass.
#include "waylaunch/content/config.h"
#include "waylaunch/content/indexer.h"
#include "waylaunch/content/store.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace waylaunch::content;
using namespace std::chrono_literals;

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  FAIL: %s\n", m); failures++; } \
                         else std::printf("  ok: %s\n", m); } while (0)

static void wf(const fs::path& p, const std::string& c) {
    std::ofstream f(p, std::ios::binary); f << c; f.flush();
}
static bool wait_for(Store& r, const std::string& q, int want, int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if ((int)r.search(q, 10).size() >= want) return true;
        std::this_thread::sleep_for(50ms);
    }
    return (int)r.search(q, 10).size() >= want;
}
static bool wait_gone(Store& r, const std::string& q, int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (r.search(q, 10).empty()) return true;
        std::this_thread::sleep_for(50ms);
    }
    return r.search(q, 10).empty();
}

int main() {
    fs::path base = fs::temp_directory_path() / ("wl_fresh_test_" + std::to_string(::getpid()));
    fs::remove_all(base);
    fs::create_directories(base);
    std::string db = (base / "index.db").string();

    ContentConfig cfg;
    cfg.roots = {base.string()};
    cfg.exclude_paths = {};
    cfg.match = MatchMode::Prefix;
    cfg.reconcile_interval_s = 1;            // periodic backstop every ~1s
    cfg.reconcile_interval_degraded_s = 1;

    Store writer;
    CHECK(writer.open(db, {false, MatchMode::Prefix}), "open writer");
    Indexer indexer(writer, cfg);
    indexer.start();                          // initial pass over the empty dir
    std::this_thread::sleep_for(150ms);

    Store reader;
    CHECK(reader.open(db, {true, MatchMode::Prefix}), "open reader");

    // Create a file WITHOUT enqueuing it and WITHOUT any watcher — only the
    // periodic reconcile can discover it.
    wf(base / "ledger.txt", "the reconcile pass finds this aardvark");
    CHECK(wait_for(reader, "aardvark", 1, 5000),
          "create → indexed by periodic reconcile (no watcher/enqueue)");

    // Modify it — new content must replace old, again via reconcile only.
    wf(base / "ledger.txt", "rewritten to mention narwhal instead");
    CHECK(wait_for(reader, "narwhal", 1, 5000), "modify → new content via reconcile");
    CHECK(wait_gone(reader, "aardvark", 5000), "modify → old content gone via reconcile");

    // Delete it — reconcile_deletions must drop it.
    std::error_code ec;
    fs::remove(base / "ledger.txt", ec);
    CHECK(wait_gone(reader, "narwhal", 5000), "delete → removed by periodic reconcile");

    // Snapshot exposes the backstop state for the control channel.
    auto snap = indexer.snapshot();
    CHECK(snap.reconcile_interval_s == 1, "snapshot reports the effective interval");
    CHECK(snap.last_reconcile_ago_s >= 0, "snapshot reports a completed reconcile");

    // Degraded switch (watch-limit) latches and is reflected in the snapshot.
    indexer.set_watch_degraded(true);
    std::this_thread::sleep_for(150ms);
    CHECK(indexer.snapshot().watch_degraded, "set_watch_degraded reflected in snapshot");

    indexer.stop();
    reader.close();
    writer.close();
    fs::remove_all(base);

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
