#include "waylaunch/content/store.h"

#include <sqlite3.h>

#include <cctype>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <sys/stat.h>

namespace waylaunch::content {

namespace {

constexpr int kSchemaVersion = 1;

// ------------------------------------------------------------------------
// Custom FTS5 tokenizer "wl": wraps unicode61 and additionally splits ASCII
// CamelCase and letter/digit runs, so OneTargetTwo / one_target_two /
// one-target-two all yield the tokens {one,target,two}. unicode61 already
// treats '_' and '-' as separators; this adds the CamelCase case it cannot do.
// (Validated end-to-end before adopting — see docs/CONTENT_SEARCH.md §6.)
// ------------------------------------------------------------------------
struct WlTok {
    fts5_tokenizer parent;        // unicode61
    Fts5Tokenizer* parent_inst;   // its instance
};

struct Tramp {
    void* pCtx;
    int (*xToken)(void*, int, const char*, int, int, int);
    const char* pText;
};

enum CharClass { CC_UPPER, CC_LOWER, CC_DIGIT, CC_OTHER };

int char_class(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return CC_UPPER;
    if (c >= 'a' && c <= 'z') return CC_LOWER;
    if (c >= '0' && c <= '9') return CC_DIGIT;
    return CC_OTHER;
}

bool is_ascii(const char* s, int n) {
    for (int i = 0; i < n; i++)
        if (static_cast<unsigned char>(s[i]) >= 0x80) return false;
    return true;
}

// A word boundary sits BEFORE index i (1..n-1) of an all-ASCII token.
bool is_boundary(const char* s, int n, int i) {
    int p = char_class(static_cast<unsigned char>(s[i - 1]));
    int c = char_class(static_cast<unsigned char>(s[i]));
    if (p == CC_LOWER && c == CC_UPPER) return true;                       // camelCase
    if (p == CC_UPPER && c == CC_UPPER && i + 1 < n &&
        char_class(static_cast<unsigned char>(s[i + 1])) == CC_LOWER)      // HTMLParser
        return true;
    if ((p == CC_UPPER || p == CC_LOWER) && c == CC_DIGIT) return true;    // letter→digit
    if (p == CC_DIGIT && (c == CC_UPPER || c == CC_LOWER)) return true;    // digit→letter
    return false;
}

int wl_trampoline(void* pC, int tflags, const char* pTok, int nTok, int iStart, int iEnd) {
    Tramp* t = static_cast<Tramp*>(pC);
    // Always emit the whole unicode61 token first (non-colocated).
    int rc = t->xToken(t->pCtx, tflags, pTok, nTok, iStart, iEnd);
    if (rc != SQLITE_OK) return rc;

    const char* o = t->pText + iStart;
    int n = iEnd - iStart;
    if (n <= 1 || !is_ascii(o, n)) return SQLITE_OK;

    // Emit each CamelCase/letter-digit segment as a colocated sub-token.
    char buf[256];
    int seg_start = 0;
    for (int i = 1; i <= n; i++) {
        if (i == n || is_boundary(o, n, i)) {
            bool whole = (seg_start == 0 && i == n);
            int len = i - seg_start;
            if (len > 0 && !whole) {
                int m = len < static_cast<int>(sizeof(buf)) ? len : static_cast<int>(sizeof(buf));
                for (int k = 0; k < m; k++)
                    buf[k] = static_cast<char>(std::tolower(static_cast<unsigned char>(o[seg_start + k])));
                rc = t->xToken(t->pCtx, FTS5_TOKEN_COLOCATED, buf, m,
                               iStart + seg_start, iStart + i);
                if (rc != SQLITE_OK) return rc;
            }
            seg_start = i;
        }
    }
    return SQLITE_OK;
}

int wl_create(void* pCtx, const char** azArg, int nArg, Fts5Tokenizer** ppOut) {
    fts5_api* api = static_cast<fts5_api*>(pCtx);
    WlTok* w = static_cast<WlTok*>(sqlite3_malloc(sizeof(WlTok)));
    if (!w) return SQLITE_NOMEM;
    std::memset(w, 0, sizeof(*w));
    void* parent_ctx = nullptr;
    int rc = api->xFindTokenizer(api, "unicode61", &parent_ctx, &w->parent);
    if (rc != SQLITE_OK) { sqlite3_free(w); return rc; }
    rc = w->parent.xCreate(parent_ctx, azArg, nArg, &w->parent_inst);
    if (rc != SQLITE_OK) { sqlite3_free(w); return rc; }
    *ppOut = reinterpret_cast<Fts5Tokenizer*>(w);
    return SQLITE_OK;
}

void wl_delete(Fts5Tokenizer* p) {
    WlTok* w = reinterpret_cast<WlTok*>(p);
    if (w->parent_inst && w->parent.xDelete) w->parent.xDelete(w->parent_inst);
    sqlite3_free(w);
}

int wl_tokenize(Fts5Tokenizer* p, void* pCtx, int flags, const char* pText, int nText,
                int (*xToken)(void*, int, const char*, int, int, int)) {
    WlTok* w = reinterpret_cast<WlTok*>(p);
    Tramp t{pCtx, xToken, pText};
    return w->parent.xTokenize(w->parent_inst, &t, flags, pText, nText, wl_trampoline);
}

fts5_api* fts5_api_from_db(sqlite3* db) {
    fts5_api* api = nullptr;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT fts5(?1)", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_pointer(st, 1, &api, "fts5_api_ptr", nullptr);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    return api;
}

bool register_tokenizer(sqlite3* db) {
    fts5_api* api = fts5_api_from_db(db);
    if (!api) return false;
    static fts5_tokenizer tk{wl_create, wl_delete, wl_tokenize};
    return api->xCreateTokenizer(api, "wl", api, &tk, nullptr) == SQLITE_OK;
}

// ------------------------------------------------------------------------
// Small SQLite helpers.
// ------------------------------------------------------------------------
bool exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK;
}

// Read a single text value (first column of first row); "" if none.
std::string scalar_text(sqlite3* db, const char* sql) {
    std::string out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(st, 0);
            if (t) out.assign(reinterpret_cast<const char*>(t));
        }
    }
    sqlite3_finalize(st);
    return out;
}

int64_t scalar_int(sqlite3* db, const char* sql) {
    int64_t out = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK)
        if (sqlite3_step(st) == SQLITE_ROW) out = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return out;
}

const char* mode_str(MatchMode m) { return m == MatchMode::Substring ? "substring" : "prefix"; }

} // namespace

// ============================================================================
// Store
// ============================================================================

Store::~Store() { close(); }

Store::Store(Store&& o) noexcept
    : db_(o.db_), path_(std::move(o.path_)), match_(o.match_), read_only_(o.read_only_) {
    o.db_ = nullptr;
}

Store& Store::operator=(Store&& o) noexcept {
    if (this != &o) {
        close();
        db_ = o.db_;
        path_ = std::move(o.path_);
        match_ = o.match_;
        read_only_ = o.read_only_;
        o.db_ = nullptr;
    }
    return *this;
}

void Store::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Store::set_pragmas(const StoreOptions& opts) {
    sqlite3_busy_timeout(db_, 3000);
    if (opts.read_only) {
        // Never mutate logical content, but keep enough capability to read a live
        // WAL index. If the daemon is up the shm exists and this is trivially fine.
        exec(db_, "PRAGMA query_only=1;");
        return true;
    }
    return exec(db_, "PRAGMA journal_mode=WAL;") &&
           exec(db_, "PRAGMA synchronous=NORMAL;") &&
           exec(db_, "PRAGMA foreign_keys=ON;") &&
           exec(db_, "PRAGMA temp_store=MEMORY;") &&
           // Bounded page cache keeps daemon RSS in budget (NFR4): ~8MB.
           exec(db_, "PRAGMA cache_size=-8000;");
}

bool Store::self_check_or_rebuild() {
    // Only meaningful for an existing, writable DB. A fresh/empty DB checks "ok".
    std::string res = scalar_text(db_, "PRAGMA quick_check;");
    if (res == "ok" || res.empty()) return true;
    // Corrupt: the index is derived data, so drop the file and start clean.
    close();
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_ + "-wal", ec);
    std::filesystem::remove(path_ + "-shm", ec);
    return sqlite3_open_v2(path_.c_str(), &db_,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK;
}

bool Store::apply_schema() {
    const char* base =
        "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE IF NOT EXISTS files("
        "  id INTEGER PRIMARY KEY,"
        "  path TEXT UNIQUE NOT NULL,"
        "  name TEXT NOT NULL,"
        "  parent TEXT NOT NULL,"
        "  size INTEGER NOT NULL,"
        "  mtime INTEGER NOT NULL,"
        "  mime TEXT,"
        "  content_hash BLOB,"
        "  indexed_at INTEGER NOT NULL,"
        "  state INTEGER NOT NULL,"
        "  body TEXT);"
        "CREATE INDEX IF NOT EXISTS files_state ON files(state);"
        "CREATE INDEX IF NOT EXISTS files_parent ON files(parent);";
    if (!exec(db_, base)) return false;

    // The FTS virtual table + sync triggers. Tokenizer/prefix depend on match mode.
    std::string fts;
    if (match_ == MatchMode::Substring) {
        fts =
            "CREATE VIRTUAL TABLE IF NOT EXISTS files_fts USING fts5("
            "  name, body, content='files', content_rowid='id',"
            "  tokenize='trigram');";
    } else {
        fts =
            "CREATE VIRTUAL TABLE IF NOT EXISTS files_fts USING fts5("
            "  name, body, content='files', content_rowid='id',"
            "  tokenize='wl remove_diacritics 2', prefix='2 3');";
    }
    if (!exec(db_, fts.c_str())) return false;

    const char* triggers =
        "CREATE TRIGGER IF NOT EXISTS files_ai AFTER INSERT ON files BEGIN"
        "  INSERT INTO files_fts(rowid,name,body) VALUES(new.id,new.name,new.body);"
        "END;"
        "CREATE TRIGGER IF NOT EXISTS files_ad AFTER DELETE ON files BEGIN"
        "  INSERT INTO files_fts(files_fts,rowid,name,body) VALUES('delete',old.id,old.name,old.body);"
        "END;"
        // Only re-index when the indexed columns change, so a metadata-only
        // touch() (mtime/size) stays cheap (FR7).
        "CREATE TRIGGER IF NOT EXISTS files_au AFTER UPDATE OF name, body ON files BEGIN"
        "  INSERT INTO files_fts(files_fts,rowid,name,body) VALUES('delete',old.id,old.name,old.body);"
        "  INSERT INTO files_fts(rowid,name,body) VALUES(new.id,new.name,new.body);"
        "END;";
    if (!exec(db_, triggers)) return false;

    // Auto-merge segments during writes (our transient→static housekeeping).
    exec(db_, "INSERT INTO files_fts(files_fts,rank) VALUES('automerge',4);");

    std::string meta =
        "INSERT INTO meta(key,value) VALUES('match','" + std::string(mode_str(match_)) +
        "') ON CONFLICT(key) DO UPDATE SET value=excluded.value;"
        "INSERT INTO meta(key,value) VALUES('tokenizer','wl+unicode61+camel')"
        " ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
    if (!exec(db_, meta.c_str())) return false;

    std::string ver = "PRAGMA user_version=" + std::to_string(kSchemaVersion) + ";";
    return exec(db_, ver.c_str());
}

bool Store::ensure_match_mode(MatchMode requested) {
    // If an existing store used a different match mode (different tokenizer/table),
    // rebuild the schema objects — the index is derived and cheap to regenerate.
    std::string stored = scalar_text(
        db_, "SELECT value FROM meta WHERE key='match';");
    bool has_table = scalar_int(
        db_, "SELECT count(*) FROM sqlite_master WHERE name='files_fts';") > 0;
    if (has_table && stored == mode_str(requested)) {
        match_ = requested;
        return true;
    }
    if (has_table) {
        exec(db_, "DROP TRIGGER IF EXISTS files_ai;"
                  "DROP TRIGGER IF EXISTS files_ad;"
                  "DROP TRIGGER IF EXISTS files_au;"
                  "DROP TABLE IF EXISTS files_fts;"
                  "DROP TABLE IF EXISTS files;");
    }
    match_ = requested;
    return apply_schema();
}

bool Store::open(const std::string& db_path, const StoreOptions& opts) {
    close();
    path_ = db_path;
    read_only_ = opts.read_only;
    match_ = opts.match;

    if (!opts.read_only) {
        // Ensure a 0700 data dir exists (NFR7).
        std::error_code ec;
        std::filesystem::path p(db_path);
        std::filesystem::create_directories(p.parent_path(), ec);
        if (!p.parent_path().empty()) ::chmod(p.parent_path().c_str(), 0700);
    }

    int flags = opts.read_only ? SQLITE_OPEN_READWRITE
                               : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    int rc = sqlite3_open_v2(db_path.c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK && opts.read_only) {
        // Read-only fallback (e.g. read-only filesystem / perms).
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        rc = sqlite3_open_v2(db_path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr);
    }
    if (rc != SQLITE_OK) { close(); return false; }

    if (!register_tokenizer(db_)) { close(); return false; }
    if (!set_pragmas(opts)) { close(); return false; }

    if (opts.read_only) {
        // Adopt whatever mode the writer built; the table must exist to be usable.
        if (scalar_int(db_, "SELECT count(*) FROM sqlite_master WHERE name='files_fts';") == 0) {
            close();
            return false;   // no index yet → caller degrades to filename search
        }
        std::string stored = scalar_text(db_, "SELECT value FROM meta WHERE key='match';");
        match_ = (stored == "substring") ? MatchMode::Substring : MatchMode::Prefix;
        return true;
    }

    if (!self_check_or_rebuild()) { close(); return false; }
    // (a rebuild may have replaced the handle; pragmas are cheap to reassert)
    set_pragmas(opts);
    if (!ensure_match_mode(opts.match)) { close(); return false; }

    // Lock down the DB file itself (NFR7).
    ::chmod(db_path.c_str(), 0600);
    return true;
}

bool Store::begin() {
    if (!db_ || read_only_) return false;
    if (sqlite3_get_autocommit(db_) == 0) return true;   // already in a transaction
    return exec(db_, "BEGIN;");
}

bool Store::commit() {
    if (!db_ || read_only_) return false;
    if (sqlite3_get_autocommit(db_) != 0) return true;   // nothing open
    return exec(db_, "COMMIT;");
}

bool Store::touch(const std::string& path, int64_t mtime_ns, int64_t size) {
    if (!db_ || read_only_) return false;
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "UPDATE files SET mtime=?2, size=?3, indexed_at=?4, state=0 WHERE path=?1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, mtime_ns);
    sqlite3_bind_int64(st, 3, size);
    sqlite3_bind_int64(st, 4, static_cast<int64_t>(::time(nullptr)));
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok && sqlite3_changes(db_) > 0;
}

bool Store::put(const FileRecord& rec, const std::string& body) {
    if (!db_ || read_only_) return false;
    const char* sql =
        "INSERT INTO files(path,name,parent,size,mtime,mime,content_hash,indexed_at,state,body)"
        " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)"
        " ON CONFLICT(path) DO UPDATE SET"
        "   name=excluded.name, parent=excluded.parent, size=excluded.size,"
        "   mtime=excluded.mtime, mime=excluded.mime, content_hash=excluded.content_hash,"
        "   indexed_at=excluded.indexed_at, state=excluded.state, body=excluded.body;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, rec.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, rec.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, rec.parent.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, rec.size);
    sqlite3_bind_int64(st, 5, rec.mtime_ns);
    if (rec.mime.empty()) sqlite3_bind_null(st, 6);
    else sqlite3_bind_text(st, 6, rec.mime.c_str(), -1, SQLITE_TRANSIENT);
    if (rec.content_hash.empty()) sqlite3_bind_null(st, 7);
    else sqlite3_bind_blob(st, 7, rec.content_hash.data(),
                           static_cast<int>(rec.content_hash.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 8, rec.indexed_at ? rec.indexed_at : static_cast<int64_t>(::time(nullptr)));
    sqlite3_bind_int(st, 9, static_cast<int>(rec.state));
    sqlite3_bind_text(st, 10, body.c_str(), static_cast<int>(body.size()), SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool Store::remove(const std::string& path) {
    if (!db_ || read_only_) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM files WHERE path=?1;", -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok && sqlite3_changes(db_) > 0;
}

bool Store::tombstone(const std::string& path) {
    if (!db_ || read_only_) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE files SET state=2 WHERE path=?1;", -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok && sqlite3_changes(db_) > 0;
}

std::optional<FileRecord> Store::get(const std::string& path) {
    if (!db_) return std::nullopt;
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT id,size,mtime,mime,content_hash,indexed_at,state FROM files WHERE path=?1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<FileRecord> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        FileRecord r;
        r.path = path;
        r.id = sqlite3_column_int64(st, 0);
        r.size = sqlite3_column_int64(st, 1);
        r.mtime_ns = sqlite3_column_int64(st, 2);
        if (const unsigned char* m = sqlite3_column_text(st, 3))
            r.mime.assign(reinterpret_cast<const char*>(m));
        if (const void* h = sqlite3_column_blob(st, 4))
            r.content_hash.assign(static_cast<const char*>(h), sqlite3_column_bytes(st, 4));
        r.indexed_at = sqlite3_column_int64(st, 5);
        r.state = static_cast<FileState>(sqlite3_column_int(st, 6));
        out = std::move(r);
    }
    sqlite3_finalize(st);
    return out;
}

bool Store::for_each_indexed_path(const std::function<void(const std::string&)>& fn) {
    if (!db_) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT path FROM files WHERE state!=2;", -1, &st, nullptr) != SQLITE_OK)
        return false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (const unsigned char* p = sqlite3_column_text(st, 0))
            fn(reinterpret_cast<const char*>(p));
    }
    sqlite3_finalize(st);
    return true;
}

bool Store::clear() {
    if (!db_ || read_only_) return false;
    exec(db_, "DELETE FROM files;");
    exec(db_, "INSERT INTO files_fts(files_fts) VALUES('rebuild');");
    return true;
}

bool Store::maintain() {
    if (!db_ || read_only_) return false;
    // Purge tombstones (delete triggers clean the FTS entries), then merge segments.
    exec(db_, "DELETE FROM files WHERE state=2;");
    exec(db_, "INSERT INTO files_fts(files_fts,rank) VALUES('merge',16);");
    exec(db_, "PRAGMA wal_checkpoint(PASSIVE);");
    return true;
}

std::string Store::build_match_query(const std::string& user_query) const {
    // Turn free text into a safe FTS5 MATCH expression: quote each whitespace-
    // separated term (implicit AND). In prefix mode the final term gets a trailing
    // '*' for as-you-type completion; in substring mode terms shorter than a
    // trigram can't match, so drop them.
    std::vector<std::string> terms;
    std::istringstream iss(user_query);
    std::string tok;
    while (iss >> tok) {
        std::string esc;
        for (char c : tok) { if (c == '"') esc += "\"\""; else esc += c; }
        if (esc.empty()) continue;
        if (match_ == MatchMode::Substring && esc.size() < 3) continue;
        terms.push_back("\"" + esc + "\"");
    }
    if (terms.empty()) return "";
    if (match_ == MatchMode::Prefix) terms.back() += "*";
    std::string out;
    for (size_t i = 0; i < terms.size(); i++) {
        if (i) out += ' ';
        out += terms[i];
    }
    return out;
}

std::vector<ContentHit> Store::search(const std::string& query, int limit,
                                      const std::string& hl_open, const std::string& hl_close) {
    std::vector<ContentHit> hits;
    if (!db_ || limit <= 0) return hits;
    std::string match = build_match_query(query);
    if (match.empty()) return hits;

    // snippet() markers and window are baked into the SQL; column 1 is `body`.
    std::string sql =
        "SELECT f.path, f.name, f.mime,"
        "       snippet(files_fts,1,?2,?3,'…',12) AS snip,"
        "       bm25(files_fts,10.0,1.0) AS score "
        "FROM files_fts JOIN files f ON f.id=files_fts.rowid "
        "WHERE files_fts MATCH ?1 AND f.state=0 "
        "ORDER BY score LIMIT ?4;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return hits;
    sqlite3_bind_text(st, 1, match.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, hl_open.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, hl_close.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 4, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        ContentHit h;
        if (const unsigned char* p = sqlite3_column_text(st, 0))
            h.path.assign(reinterpret_cast<const char*>(p));
        if (const unsigned char* n = sqlite3_column_text(st, 1))
            h.name.assign(reinterpret_cast<const char*>(n));
        if (const unsigned char* m = sqlite3_column_text(st, 2))
            h.mime.assign(reinterpret_cast<const char*>(m));
        if (const unsigned char* s = sqlite3_column_text(st, 3))
            h.snippet.assign(reinterpret_cast<const char*>(s));
        h.score = -sqlite3_column_double(st, 4);   // negate: SQLite bm25 is lower-is-better
        hits.push_back(std::move(h));
    }
    sqlite3_finalize(st);
    return hits;
}

StoreStats Store::stats() {
    StoreStats s;
    if (!db_) return s;
    s.total = scalar_int(db_, "SELECT count(*) FROM files;");
    s.indexed = scalar_int(db_, "SELECT count(*) FROM files WHERE state=0;");
    s.pending = scalar_int(db_, "SELECT count(*) FROM files WHERE state=1;");
    s.tombstoned = scalar_int(db_, "SELECT count(*) FROM files WHERE state=2;");
    s.errored = scalar_int(db_, "SELECT count(*) FROM files WHERE state=3;");
    s.db_bytes = scalar_int(db_, "SELECT page_count*page_size FROM pragma_page_count(),pragma_page_size();");
    return s;
}

} // namespace waylaunch::content
