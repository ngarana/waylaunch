// waylaunchd — the content-search indexing daemon (the mds analog).
// Loads [content] config, owns the read-write index Store, runs the throttled
// Indexer + inotify FsWatcher, and serves a control socket. Queries never touch
// this process: the launcher reads the index file directly (WAL).

#include "waylaunch/content/config.h"
#include "waylaunch/content/control.h"
#include "waylaunch/content/fs_watcher.h"
#include "waylaunch/content/indexer.h"
#include "waylaunch/content/store.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace waylaunch::content;

namespace {

int g_shutdown_fd = -1;

void on_signal(int) {
    if (g_shutdown_fd >= 0) {
        uint64_t one = 1;
        ssize_t w = write(g_shutdown_fd, &one, sizeof(one));   // async-signal-safe
        (void)w;
    }
}

std::string yesno(bool b) { return b ? "yes" : "no"; }

std::string build_status(const ContentConfig& cfg, Indexer& indexer, FsWatcher& watcher,
                         Store& reader) {
    auto s = indexer.snapshot();
    StoreStats st = reader.is_open() ? reader.stats() : StoreStats{};
    std::string roots;
    for (size_t i = 0; i < cfg.roots.size(); i++) roots += (i ? "," : "") + cfg.roots[i];
    std::string out;
    out += "running: yes\n";
    out += "paused: " + yesno(s.paused) + "\n";
    out += "crawling: " + yesno(s.crawling) + "\n";
    out += "match: " + std::string(cfg.match == MatchMode::Substring ? "substring" : "prefix") + "\n";
    out += "db: " + ContentConfig::db_path() + "\n";
    out += "db_bytes: " + std::to_string(st.db_bytes) + "\n";
    out += "roots: " + roots + "\n";
    out += "total_files: " + std::to_string(st.total) + "\n";
    out += "indexed_files: " + std::to_string(st.indexed) + "\n";
    out += "error_files: " + std::to_string(st.errored) + "\n";
    out += "indexed_ops: " + std::to_string(s.indexed) + "\n";
    out += "errors: " + std::to_string(s.errors) + "\n";
    out += "queued: " + std::to_string(s.queued) + "\n";
    out += "watches: " + std::to_string(watcher.watch_count()) + "\n";
    out += "watch_limit_hit: " + yesno(watcher.watch_limit_hit()) + "\n";
    return out;
}

void print_help() {
    std::printf(
        "waylaunchd — waylaunch content-search indexer\n\n"
        "Usage: waylaunchd [options]\n\n"
        "  -c, --config PATH   config.toml to read [content] from\n"
        "      --once          crawl + reconcile once, then exit (cron/manual)\n"
        "      --reindex       drop the index and rebuild from scratch\n"
        "  -h, --help          show this help\n"
        "      --version       print version\n\n"
        "Runs in the foreground; wire it up as a systemd --user service for autostart.\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path;
    bool once = false, reindex = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if ((a == "-c" || a == "--config") && i + 1 < argc) config_path = argv[++i];
        else if (a == "--once") once = true;
        else if (a == "--reindex") reindex = true;
        else if (a == "-h" || a == "--help") { print_help(); return 0; }
        else if (a == "--version") { std::printf("waylaunchd 0.1.0\n"); return 0; }
        else { std::fprintf(stderr, "waylaunchd: unknown option '%s'\n", a.c_str()); return 2; }
    }

    signal(SIGPIPE, SIG_IGN);

    ContentConfig cfg = load_content_config(config_path);
    if (!cfg.enable) {
        std::fprintf(stderr, "waylaunchd: content search disabled in config; exiting\n");
        return 0;
    }

    // Private runtime + data dirs.
    std::error_code ec;
    fs::create_directories(ContentConfig::runtime_dir(), ec);
    ::chmod(ContentConfig::runtime_dir().c_str(), 0700);

    std::string db = ContentConfig::db_path();
    Store store;
    if (!store.open(db, {false, cfg.match})) {
        std::fprintf(stderr, "waylaunchd: cannot open index at %s\n", db.c_str());
        return 1;
    }

    Indexer indexer(store, cfg);
    if (reindex) indexer.request_reindex();

    if (once) {
        indexer.run_once();
        auto s = indexer.snapshot();
        std::fprintf(stderr, "waylaunchd: --once done (indexed_ops=%lld errors=%lld)\n",
                     (long long)s.indexed, (long long)s.errors);
        return 0;
    }

    // Single-instance lock (per user).
    std::string lockpath = ContentConfig::runtime_dir() + "/waylaunchd.lock";
    int lockfd = open(lockpath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
        std::fprintf(stderr, "waylaunchd: another instance is already running\n");
        return 1;
    }

    g_shutdown_fd = eventfd(0, EFD_CLOEXEC);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    indexer.start();

    FsWatcher watcher(cfg.roots, [&](const std::string& p) { return indexer.is_excluded(p); });
    watcher.start({
        [&](const std::string& p) { indexer.enqueue_index(p); },
        [&](const std::string& p) { indexer.enqueue_remove(p); },
        [&] { indexer.request_reconcile(); },
    });

    // A dedicated read-only handle for status queries (separate from the writer).
    Store status_reader;
    status_reader.open(db, {true, cfg.match});

    std::atomic<bool> want_shutdown{false};
    ControlServer control(ContentConfig::socket_path());
    bool control_ok = control.start([&](const std::string& cmd, const std::string& arg) -> std::string {
        if (cmd == "ping") return "pong\n";
        if (cmd == "status") return build_status(cfg, indexer, watcher, status_reader);
        if (cmd == "pause") { indexer.set_paused(true); return "ok: paused\n"; }
        if (cmd == "resume") { indexer.set_paused(false); return "ok: resumed\n"; }
        if (cmd == "reindex") { indexer.request_reindex(); return "ok: reindexing\n"; }
        if (cmd == "reconcile") { indexer.request_reconcile(); return "ok: reconciling\n"; }
        if (cmd == "exclude") {
            if (arg.empty()) return "error: exclude needs a path\n";
            indexer.add_runtime_exclude(expand_tilde(arg));
            return "ok: excluded " + arg + "\n";
        }
        if (cmd == "shutdown") {
            want_shutdown.store(true);
            uint64_t one = 1;
            ssize_t w = write(g_shutdown_fd, &one, sizeof(one));
            (void)w;
            return "ok: shutting down\n";
        }
        return "error: unknown command '" + cmd + "'\n";
    });

    if (!control_ok)
        std::fprintf(stderr, "waylaunchd: WARNING control socket unavailable (%s) — "
                     "indexing continues, but waylaunchctl won't connect\n",
                     ContentConfig::socket_path().c_str());
    std::fprintf(stderr, "waylaunchd: indexing %zu root(s); socket %s\n",
                 cfg.roots.size(), ContentConfig::socket_path().c_str());

    // Wait for a shutdown signal / control command.
    for (;;) {
        pollfd pfd{g_shutdown_fd, POLLIN, 0};
        int pr = poll(&pfd, 1, -1);
        if (pr < 0 && errno == EINTR) continue;
        break;
    }

    std::fprintf(stderr, "waylaunchd: shutting down\n");
    control.stop();
    watcher.stop();
    indexer.stop();
    status_reader.close();
    store.close();
    flock(lockfd, LOCK_UN);
    close(lockfd);
    if (g_shutdown_fd >= 0) close(g_shutdown_fd);
    return 0;
}
