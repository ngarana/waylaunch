// Store correctness: schema, upsert, tokenizer, remove/tombstone/maintain,
// read-only degradation, substring-mode rebuild, cross-form token equivalence,
// query precision, snippet-on-original, availability classification, subtree
// removal, migration self-heal, and the bounded worst-case query planner.
#include "waylaunch/content/store.h"

#include <sqlite3.h>

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

    // ---- cross-form token equivalence (challenge §1) ----------------------
    // The three boundary-bearing spellings (CamelCase / snake / kebab) must all
    // yield {one,target,two}, so a query in any of those spellings finds a
    // document written in any other. Flat lowercase "onetargettwo" carries no
    // boundary signal — like Spotlight, we can't split it, and it's covered
    // separately by its own compound token.
    {
        std::string edb = (dir / "equiv.db").string();
        Store e;
        CHECK(e.open(edb, {false, MatchMode::Prefix}), "equiv: open");
        e.put(rec(dir / "f_camel.txt", "f_camel.txt"),  "the OneTargetTwo body");
        e.put(rec(dir / "f_snake.txt", "f_snake.txt"),  "the one_target_two body");
        e.put(rec(dir / "f_kebab.txt", "f_kebab.txt"),  "the one-target-two body");
        e.put(rec(dir / "f_flat.txt",  "f_flat.txt"),   "the onetargettwo body");
        // The single shared word all three boundary forms split to.
        CHECK(e.search("target", 9).size() == 3, "equiv: 'target' finds the 3 boundary forms");
        // Each boundary spelling as a query resolves to the phrase one+target+two
        // and matches all three boundary docs.
        CHECK(e.search("OneTargetTwo", 9).size() == 3, "equiv: camel query matches all 3");
        CHECK(e.search("one_target_two", 9).size() == 3, "equiv: snake query matches all 3");
        CHECK(e.search("one-target-two", 9).size() == 3, "equiv: kebab query matches all 3");
        // The flat lowercase compound matches the flat doc AND the CamelCase doc
        // (whose whole-compound token is indexed colocated), but not snake/kebab
        // — unicode61 already split those on '_'/'-', so no compound survives.
        CHECK(e.search("onetargettwo", 9).size() == 2, "equiv: flat compound matches flat + camel");
        e.close();
    }

    // ---- query precision regression (the colocated-at-query bug) ----------
    // A CamelCase query must be a PHRASE, not an OR of its segments: querying
    // "OneTargetTwo" must NOT match a doc that merely contains the word "one".
    {
        std::string pdb = (dir / "prec.db").string();
        Store p;
        CHECK(p.open(pdb, {false, MatchMode::Prefix}), "precision: open");
        p.put(rec(dir / "hit.txt", "hit.txt"),  "look at OneTargetTwo here");
        p.put(rec(dir / "miss.txt", "miss.txt"), "just one lonely word");
        auto r2 = p.search("OneTargetTwo", 9);
        CHECK(r2.size() == 1, "precision: camel query matches only the real compound");
        CHECK(!r2.empty() && fs::path(r2[0].path).filename() == "hit.txt",
              "precision: the match is the compound doc, not the 'one' doc");
        p.close();
    }

    // ---- snippet reads ORIGINAL (decompressed) text (challenge §1.4/§3) ----
    // Tokens are folded, but the snippet must highlight original-cased bytes,
    // proving snippet() reads real text through the compressed doc view.
    {
        std::string sdb = (dir / "snip.db").string();
        Store s;
        CHECK(s.open(sdb, {false, MatchMode::Prefix}), "snippet: open");
        s.put(rec(dir / "q.txt", "q.txt"), "Annual Quarterly Revenue Report FY2026");
        auto r3 = s.search("revenue", 5, "[", "]");
        CHECK(!r3.empty() && r3[0].snippet.find("Revenue") != std::string::npos,
              "snippet: preserves original casing 'Revenue'");
        CHECK(!r3.empty() && r3[0].snippet.find("[Revenue]") != std::string::npos,
              "snippet: wraps the match with the given markers");
        s.close();
    }

    // ---- subtree removal (challenge §6: moved/deleted directories) ---------
    {
        std::string tdb = (dir / "tree.db").string();
        Store t;
        CHECK(t.open(tdb, {false, MatchMode::Prefix}), "subtree: open");
        t.put(rec(dir / "proj/a.txt",       "a.txt"),  "alpha content"); // note: rec derives parent
        t.put(rec(dir / "proj/sub/b.txt",   "b.txt"),  "bravo content");
        t.put(rec(dir / "proj2/c.txt",      "c.txt"),  "charlie content");
        CHECK(t.stats().total == 3, "subtree: 3 files indexed");
        CHECK(t.remove_subtree((dir / "proj").string()), "subtree: remove proj/");
        CHECK(t.search("alpha", 5).empty(), "subtree: nested file dropped");
        CHECK(t.search("bravo", 5).empty(), "subtree: deeper nested file dropped");
        CHECK(t.search("charlie", 5).size() == 1, "subtree: sibling prefix proj2/ untouched");
        CHECK(t.stats().total == 1, "subtree: only sibling remains");
        t.close();
    }

    // ---- availability classification (challenge §5.2) ---------------------
    {
        Store a;
        CHECK(a.open((dir / "never.db").string(), {true, MatchMode::Prefix}) == false &&
                  a.availability() == Availability::NoIndex,
              "avail: absent db → NoIndex");
    }
    {
        // Tamper user_version to force a schema mismatch.
        std::string mdb = (dir / "mig.db").string();
        { Store w2; CHECK(w2.open(mdb, {false, MatchMode::Prefix}), "avail: build v-current");
          w2.put(rec(dir / "z.txt", "z.txt"), "zeta payload"); w2.close(); }
        sqlite3* raw = nullptr;
        sqlite3_open(mdb.c_str(), &raw);
        sqlite3_exec(raw, "PRAGMA user_version=999;", nullptr, nullptr, nullptr);
        sqlite3_close(raw);
        Store rdr;
        CHECK(rdr.open(mdb, {true, MatchMode::Prefix}) == false &&
                  rdr.availability() == Availability::VersionMismatch,
              "avail: reader on wrong version → VersionMismatch (degrade)");
        // Writer self-heals: rebuild wipes the stale index.
        Store w3;
        CHECK(w3.open(mdb, {false, MatchMode::Prefix}), "avail: writer reopens mismatched db");
        CHECK(w3.stats().total == 0, "avail: writer rebuilt (stale rows cleared)");
        Store ok;
        CHECK(ok.open(mdb, {true, MatchMode::Prefix}) && ok.availability() == Availability::Ok,
              "avail: reader Ok after writer rebuild");
        w3.close(); ok.close();
    }

    // ---- bounded worst-case planner still returns ranked results (§8) -----
    {
        std::string bdb = (dir / "bounded.db").string();
        StoreOptions bo{false, MatchMode::Prefix};
        bo.common_term_df = 1;      // force the bounded path for any repeated term
        bo.candidate_budget = 4;
        Store b;
        CHECK(b.open(bdb, bo), "bounded: open with tiny df threshold");
        for (int i = 0; i < 12; i++)
            b.put(rec(dir / ("c" + std::to_string(i) + ".txt"),
                      "c" + std::to_string(i) + ".txt"),
                  "common token here document number " + std::to_string(i));
        auto rb = b.search("common", 3);
        CHECK(rb.size() == 3, "bounded: honors the limit under the bounded planner");
        CHECK(!rb.empty() && !rb[0].snippet.empty(), "bounded: still produces snippets");
        b.close();
    }

    fs::remove_all(dir);
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
