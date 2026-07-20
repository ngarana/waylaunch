// waylaunchctl — control/inspect the content-search daemon (the mdutil analog).
// Management commands talk to waylaunchd over its Unix control socket; `search`
// queries the index file directly (read-only), so it works whether or not the
// daemon is running (NFR8), like mdfind.

#include "waylaunch/content/config.h"
#include "waylaunch/content/control.h"
#include "waylaunch/content/store.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace waylaunch::content;

namespace {
void print_help() {
    std::printf(
        "waylaunchctl — control the waylaunch content-search daemon\n\n"
        "Usage: waylaunchctl [-c CONFIG] <command> [arg]\n\n"
        "Commands:\n"
        "  status            show index/daemon status\n"
        "  search <query>    query the index directly (read-only, like mdfind)\n"
        "                    predicates: kind:pdf|image|audio|video|archive|doc|\n"
        "                                text|code|spreadsheet|presentation\n"
        "                                ext:xlsx  name:report  size:>1M  size:<500k\n"
        "                                modified:<7d  modified:>2w  modified:today\n"
        "                    e.g. `waylaunchctl search kind:pdf quarterly revenue`\n"
        "  pause             pause indexing\n"
        "  resume            resume indexing\n"
        "  reindex           drop the index and rebuild from scratch\n"
        "  reconcile         re-scan roots to catch missed changes\n"
        "  exclude <path>    stop indexing a path (and drop it) at runtime\n"
        "  shutdown          stop the daemon\n"
        "  ping              check the daemon is alive\n\n"
        "Options:\n"
        "  -c, --config PATH   config.toml to read [content] from\n");
}

// Human-readable reason an index can't answer, so `search`/degradation is
// diagnosable (challenge §5.2) rather than a bare "no index".
const char* availability_reason(Availability a) {
    switch (a) {
        case Availability::NoIndex:         return "no index yet (start waylaunchd and let it crawl)";
        case Availability::VersionMismatch: return "index built by an incompatible version (waylaunchd will rebuild)";
        case Availability::Corrupt:         return "index is corrupt (waylaunchd will rebuild)";
        case Availability::Locked:          return "index is locked or unreadable (permissions?)";
        default:                            return "index unavailable";
    }
}

// Query the index directly (no daemon round-trip) — mirrors how the launcher
// searches, so it works whether or not waylaunchd is running.
int do_search(const std::string& config_path, const std::vector<std::string>& terms) {
    std::string q;
    for (const auto& t : terms) q += (q.empty() ? "" : " ") + t;
    if (q.empty()) { std::fprintf(stderr, "waylaunchctl: search needs a query\n"); return 2; }
    ContentConfig cc = load_content_config(config_path);
    Store store;
    if (!store.open(ContentConfig::db_path(), {true, cc.match})) {
        std::fprintf(stderr, "waylaunchctl: %s\n  db: %s\n",
                     availability_reason(store.availability()),
                     ContentConfig::db_path().c_str());
        // NoIndex/VersionMismatch are expected transient states, not hard errors.
        return store.availability() == Availability::NoIndex ? 0 : 1;
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
    // Parse a leading -c/--config, then the command and its args (consistent
    // with waylaunchd, which also takes -c).
    std::string config_path;
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if ((a == "-c" || a == "--config") && i + 1 < argc) config_path = argv[++i];
        else if (a == "-h" || a == "--help") { print_help(); return 0; }
        else args.push_back(std::move(a));
    }
    if (args.empty()) { print_help(); return 2; }

    const std::string& cmd = args[0];
    if (cmd == "search")
        return do_search(config_path, {args.begin() + 1, args.end()});

    std::string line = cmd;
    for (size_t i = 1; i < args.size(); i++) line += " " + args[i];

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
