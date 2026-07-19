// Store correctness: schema, upsert, tokenizer, remove/tombstone/maintain,
// read-only degradation, substring-mode rebuild.
#include "waylaunch/content/store.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace waylaunch::content;

static int failures = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  FAIL: %s\n", m); failures++; } \
                         else std::printf("  ok: %s\n", m); } while (0)

static FileRecord rec(const std::string& path, const std::string& name) {
    FileRecord r;
    r.path = path; r.name = name; r.parent = fs::path(path).parent_path().string();
    r.size = 100; r.mtime_ns = 123; r.mime = "text/plain";
    r.content_hash = std::string("\x01\x02\x03\x04", 4);
    r.state = FileState::Indexed;
    return r;
}

int main() {
    fs::path dir = fs::temp_directory_path() / ("wl_store_test_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string db = (dir / "index.db").string();

    Store w;
    CHECK(w.open(db, {false, MatchMode::Prefix}), "open writer");
    CHECK(w.put(rec(dir / "OneTargetTwo.txt", "OneTargetTwo.txt"), "the quick brown fox report"), "put camel");
    CHECK(w.put(rec(dir / "notes_snake.md", "notes_snake.md"), "quarterly revenue one_target_two"), "put snake");
    CHECK(w.put(rec(dir / "HTMLParser.java", "HTMLParser.java"), "class getHTTPResponse parseXML2JSON"), "put java");
    CHECK(w.stats().indexed == 3, "3 indexed");

    CHECK(w.search("target", 5).size() == 2, "camel+snake both match 'target'");
    CHECK(w.search("response", 5).size() == 1, "acronym split: 'response' from getHTTPResponse");
    CHECK(w.search("quar", 5).size() == 1, "prefix 'quar' matches quarterly");
    auto snip = w.search("revenue", 5);
    CHECK(!snip.empty() && !snip[0].snippet.empty(), "snippet returned");

    // upsert replaces content
    CHECK(w.put(rec(dir / "notes_snake.md", "notes_snake.md"), "totally different now"), "re-put (upsert)");
    CHECK(w.stats().total == 3, "still 3 after upsert");
    CHECK(w.search("quar", 5).empty(), "old term gone after upsert");
    CHECK(w.search("different", 5).size() == 1, "new term present after upsert");

    CHECK(w.remove((dir / "HTMLParser.java").string()), "remove java");
    CHECK(w.search("response", 5).empty(), "removed doc no longer matches");
    CHECK(w.tombstone((dir / "OneTargetTwo.txt").string()), "tombstone camel");
    CHECK(w.search("fox", 5).empty(), "tombstoned filtered from search");
    CHECK(w.maintain(), "maintain");
    CHECK(w.stats().total == 1, "1 left after purge");
    w.close();

    Store r;
    CHECK(r.open(db, {true, MatchMode::Prefix}), "open reader");
    CHECK(r.search("different", 5).size() == 1, "reader finds survivor");
    CHECK(r.put(rec(dir / "x", "x"), "y") == false, "reader rejects writes");
    r.close();

    Store absent;
    CHECK(absent.open((dir / "nope.db").string(), {true, MatchMode::Prefix}) == false,
          "absent index → open false (degrade)");

    Store sub;
    CHECK(sub.open(db, {false, MatchMode::Substring}), "reopen substring mode");
    CHECK(sub.match_mode() == MatchMode::Substring, "mode switched");
    CHECK(sub.stats().total == 0, "substring rebuild cleared old rows");
    sub.put(rec(dir / "r.txt", "r.txt"), "the mytargetfile lives here");
    CHECK(sub.search("targetfil", 5).size() == 1, "substring infix match");
    sub.close();

    fs::remove_all(dir);
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
