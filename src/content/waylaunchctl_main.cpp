// waylaunchctl — control/inspect the content-search daemon (the mdutil analog).
// Talks to waylaunchd over its Unix control socket.

#include "waylaunch/content/config.h"
#include "waylaunch/content/control.h"
#include "waylaunch/content/store.h"

#include <cstdio>
#include <string>

using namespace waylaunch::content;

namespace {
void print_help() {
    std::printf(
        "waylaunchctl — control the waylaunch content-search daemon\n\n"
        "Usage: waylaunchctl <command> [arg]\n\n"
        "Commands:\n"
        "  status            show index/daemon status\n"
        "  search <query>    query the index directly (read-only, like mdfind)\n"
        "  pause             pause indexing\n"
        "  resume            resume indexing\n"
        "  reindex           drop the index and rebuild from scratch\n"
        "  reconcile         re-scan roots to catch missed changes\n"
        "  exclude <path>    stop indexing a path (and drop it) at runtime\n"
        "  shutdown          stop the daemon\n"
        "  ping              check the daemon is alive\n");
}

// Query the index directly (no daemon round-trip) — mirrors how the launcher
// searches, so it works whether or not waylaunchd is running.
int do_search(int argc, char** argv) {
    std::string q;
    for (int i = 2; i < argc; i++) q += (q.empty() ? "" : " ") + std::string(argv[i]);
    if (q.empty()) { std::fprintf(stderr, "waylaunchctl: search needs a query\n"); return 2; }
    ContentConfig cc = load_content_config();
    Store store;
    if (!store.open(ContentConfig::db_path(), {true, cc.match})) {
        std::fprintf(stderr, "waylaunchctl: no index yet at %s\n"
                     "  (start waylaunchd and let it crawl first)\n",
                     ContentConfig::db_path().c_str());
        return 1;
    }
    auto hits = store.search(q, cc.max_results);
    if (hits.empty()) { std::printf("(no content matches for \"%s\")\n", q.c_str()); return 0; }
    for (const auto& h : hits) {
        std::printf("%s\n", h.path.c_str());
        if (!h.snippet.empty()) std::printf("    %s\n", h.snippet.c_str());
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { print_help(); return 2; }
    std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help") { print_help(); return 0; }
    if (cmd == "search") return do_search(argc, argv);

    std::string line = cmd;
    for (int i = 2; i < argc; i++) line += " " + std::string(argv[i]);

    auto reply = control_send(ContentConfig::socket_path(), line);
    if (!reply) {
        std::fprintf(stderr,
                     "waylaunchctl: cannot reach waylaunchd (is it running?)\n"
                     "  socket: %s\n", ContentConfig::socket_path().c_str());
        return 1;
    }
    std::fputs(reply->c_str(), stdout);
    // Non-zero exit if the daemon reported an error.
    return reply->rfind("error:", 0) == 0 ? 1 : 0;
}
