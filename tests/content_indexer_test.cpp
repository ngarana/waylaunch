// Freshness (NFR2) + discovery: initial crawl, then inotify-driven
// create/modify/delete/rename/subdir, observed through a read-only reader.
#include "waylaunch/content/config.h"
#include "waylaunch/content/fs_watcher.h"
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
static bool wait_for(Store& r, const std::string& q, int want, int ms = 8000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if ((int)r.search(q, 10).size() >= want) return true;
        std::this_thread::sleep_for(40ms);
    }
    return (int)r.search(q, 10).size() >= want;
}
static bool wait_gone(Store& r, const std::string& q, int ms = 8000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (r.search(q, 10).empty()) return true;
        std::this_thread::sleep_for(40ms);
    }
    return r.search(q, 10).empty();
}

int main() {
    fs::path base = fs::temp_directory_path() / ("wl_idx_test_" + std::to_string(::getpid()));
    fs::remove_all(base);
    fs::create_directories(base / "sub");
    std::string db = (base / "index.db").string();

    wf(base / "alpha.txt", "the alpha document mentions wombat and revenue");
    wf(base / "sub" / "notes.md", "# notes\n\nabout penguins here");

    ContentConfig cfg;
    cfg.roots = {base.string()};
    cfg.exclude_paths = {};
    cfg.match = MatchMode::Prefix;

    Store writer;
    CHECK(writer.open(db, {false, MatchMode::Prefix}), "open writer");
    Indexer indexer(writer, cfg);
    indexer.start();

    std::this_thread::sleep_for(200ms);
    Store reader;
    CHECK(reader.open(db, {true, MatchMode::Prefix}), "open reader");

    CHECK(wait_for(reader, "wombat", 1), "crawl indexed alpha.txt");
    CHECK(wait_for(reader, "penguins", 1), "crawl indexed sub/notes.md");

    FsWatcher watcher({base.string()}, [&](const std::string& p) { return indexer.is_excluded(p); });
    watcher.start({[&](const std::string& p) { indexer.enqueue_index(p); },
                   [&](const std::string& p) { indexer.enqueue_remove(p); },
                   [&] { indexer.request_reconcile(); }});
    std::this_thread::sleep_for(100ms);

    wf(base / "beta.txt", "a brand new file about aardvark");
    CHECK(wait_for(reader, "aardvark", 1), "create → indexed");

    wf(base / "alpha.txt", "alpha rewritten about narwhal");
    CHECK(wait_for(reader, "narwhal", 1), "modify → new content indexed");
    CHECK(wait_gone(reader, "wombat"), "modify → old content gone");

    fs::create_directories(base / "deep" / "nested");
    wf(base / "deep" / "nested" / "buried.txt", "deeply buried mongoose secret");
    CHECK(wait_for(reader, "mongoose", 1), "new subdir file → watch added + indexed");

    std::error_code ec;
    fs::rename(base / "beta.txt", base / "gamma.txt", ec);
    wf(base / "gamma.txt", "gamma now about quetzal");
    CHECK(wait_for(reader, "quetzal", 1), "rename → indexed at new path");

    fs::remove(base / "sub" / "notes.md", ec);
    CHECK(wait_gone(reader, "penguins"), "delete → removed from index");

    watcher.stop();
    indexer.stop();
    reader.close();
    writer.close();
    fs::remove_all(base);

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
