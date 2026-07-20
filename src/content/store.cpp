#include "waylaunch/content/store.h"

#include <sqlite3.h>
#include <zstd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <sys/stat.h>

namespace waylaunch::content {

namespace {

// Bump whenever the schema, tokenizer semantics, or compression format change:
// the index is derived data, so a mismatch is handled by rebuilding (writer)
// or degrading to filename search until the daemon rebuilds (reader).
constexpr int kSchemaVersion = 2;

constexpr int    kZstdLevel    = 9;                  // ~3x text compression, fast
constexpr size_t kMaxBodyBytes = 16 * 1024 * 1024;   // decompress sanity bound

int64_t now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// ------------------------------------------------------------------------
// Custom FTS5 tokenizer "wl": wraps unicode61 and additionally segments ASCII
// CamelCase and letter/digit runs. unicode61 already splits on '_'/'-'; this
// adds the case/digit boundaries it cannot see, with acronym handling
// (XMLHttpRequest → xml http request).
//
// Emission model (challenge §1.3 — index and query must agree):
//   index: each segment at its own position (offsets into the ORIGINAL text so
//          snippet()/highlight() mark the right bytes, challenge §1.4), plus
//          the whole folded compound COLOCATED with the first segment so a
//          lowercase compound query ("onetargettwo") still matches.
//   query: segments only. A quoted term that segments becomes a phrase, so
//          "OneTargetTwo", "one_target_two", "one-target-two" all compile to
//          the phrase one+target+two and match any of the indexed forms; a
//          compound synonym here would OR against a single position and make
//          "OneTargetTwo" match documents containing just "one" (the precision
//          bug this replaces).
// ------------------------------------------------------------------------
struct WlTok {
    fts5_tokenizer parent;        // unicode61
    Fts5Tokenizer* parent_inst;   // its instance
};

struct Tramp {
    void* pCtx;
    int (*xToken)(void*, int, const char*, int, int, int);
    const char* pText;
    bool  query;                  // FTS5_TOKENIZE_QUERY
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
    const char* o = t->pText + iStart;
    int n = iEnd - iStart;

    constexpr int kMaxSegs = 63;
    int bounds[kMaxSegs];
    int nb = 0;
    if (n > 1 && n <= 256 && is_ascii(o, n)) {
        for (int i = 1; i < n && nb < kMaxSegs; i++)
            if (is_boundary(o, n, i)) bounds[nb++] = i;
    }
    if (nb == 0)
        return t->xToken(t->pCtx, tflags, pTok, nTok, iStart, iEnd);

    char buf[256];
    int seg_start = 0;
    for (int bi = 0; bi <= nb; bi++) {
        int end = (bi == nb) ? n : bounds[bi];
        int len = end - seg_start;
        for (int k = 0; k < len; k++)
            buf[k] = static_cast<char>(std::tolower(static_cast<unsigned char>(o[seg_start + k])));
        int rc = t->xToken(t->pCtx, 0, buf, len, iStart + seg_start, iStart + end);
        if (rc != SQLITE_OK) return rc;
        if (bi == 0 && !t->query) {
            rc = t->xToken(t->pCtx, FTS5_TOKEN_COLOCATED, pTok, nTok, iStart, iEnd);
            if (rc != SQLITE_OK) return rc;
        }
        seg_start = end;
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
    Tramp t{pCtx, xToken, pText, (flags & FTS5_TOKENIZE_QUERY) != 0};
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
// zstd body compression. wl_unzstd() is the SQL side: the docs_v view and the
// FTS sync triggers call it, which is how FTS5 sees original text (for
// tokenizing, 'delete' postings, and snippets) while the disk stores ~30%.
// INNOCUOUS so it stays callable from schema objects (views/triggers).
// ------------------------------------------------------------------------
void fn_unzstd(sqlite3_context* ctx, int, sqlite3_value** argv) {
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_text(ctx, "", 0, SQLITE_STATIC);
        return;
    }
    const void* z = sqlite3_value_blob(argv[0]);
    int nz = sqlite3_value_bytes(argv[0]);
    if (!z || nz <= 0) { sqlite3_result_text(ctx, "", 0, SQLITE_STATIC); return; }
    unsigned long long sz = ZSTD_getFrameContentSize(z, static_cast<size_t>(nz));
    if (sz == ZSTD_CONTENTSIZE_UNKNOWN || sz == ZSTD_CONTENTSIZE_ERROR || sz > kMaxBodyBytes) {
        sqlite3_result_error(ctx, "wl_unzstd: bad frame", -1);
        return;
    }
    char* out = static_cast<char*>(sqlite3_malloc64(sz ? sz : 1));
    if (!out) { sqlite3_result_error_nomem(ctx); return; }
    size_t r = ZSTD_decompress(out, sz, z, static_cast<size_t>(nz));
    if (ZSTD_isError(r) || r != sz) {
        sqlite3_free(out);
        sqlite3_result_error(ctx, "wl_unzstd: corrupt frame", -1);
        return;
    }
    sqlite3_result_text64(ctx, out, sz, sqlite3_free, SQLITE_UTF8);
}

bool register_functions(sqlite3* db) {
    return sqlite3_create_function_v2(
               db, "wl_unzstd", 1,
               SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
               nullptr, fn_unzstd, nullptr, nullptr, nullptr) == SQLITE_OK;
}

std::string compress_body(const std::string& body) {
    if (body.empty()) return {};
    size_t bound = ZSTD_compressBound(body.size());
    std::string out(bound, '\0');
    size_t n = ZSTD_compress(out.data(), bound, body.data(), body.size(), kZstdLevel);
    if (ZSTD_isError(n)) return {};
    out.resize(n);
    return out;
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

// Query deadline for the full ranked pass (challenge §8): a progress handler
// that trips once the monotonic clock passes the budget, interrupting the
// statement so search() can fall back to the bounded strategy.
int deadline_cb(void* p) { return now_ms() > *static_cast<int64_t*>(p); }

// SQL fragment matching `path` itself or anything under it. '0' is '/'+1, so
// [path||'/', path||'0') is exactly the half-open range of paths below it.
constexpr char kSubtreeWhere[] = "(path=?1 OR (path>=?1||'/' AND path<?1||'0'))";

} // namespace

// ============================================================================
// Store
// ============================================================================

Store::~Store() { close(); }

Store::Store(Store&& o) noexcept
    : db_(o.db_), path_(std::move(o.path_)), match_(o.match_), read_only_(o.read_only_),
      avail_(o.avail_), opts_(o.opts_) {
    o.db_ = nullptr;
}

Store& Store::operator=(Store&& o) noexcept {
    if (this != &o) {
        close();
        db_ = o.db_;
        path_ = std::move(o.path_);
        match_ = o.match_;
        read_only_ = o.read_only_;
        avail_ = o.avail_;
        opts_ = o.opts_;
        o.db_ = nullptr;
    }
    return *this;
}

void Store::close() {
    if (db_) {
        // Leave a clean, WAL-free file behind so a later strictly read-only
        // reader needs no -shm/-wal machinery at all (challenge §5).
        if (!read_only_)
            sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_TRUNCATE,
                                      nullptr, nullptr);
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Store::open_handle(int flags) {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
    if (sqlite3_open_v2(path_.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return false;
    }
    sqlite3_busy_timeout(db_, 3000);
    if (!register_tokenizer(db_) || !register_functions(db_)) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    return true;
}

bool Store::recreate_db() {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_ + "-wal", ec);
    std::filesystem::remove(path_ + "-shm", ec);
    return open_handle(SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) && set_pragmas();
}

bool Store::set_pragmas() {
    if (read_only_) {
        // Belt over the read-only open flag: even a fallback read-write handle
        // must never mutate logical content.
        exec(db_, "PRAGMA query_only=1;");
        return true;
    }
    return exec(db_, "PRAGMA journal_mode=WAL;") &&
           exec(db_, "PRAGMA synchronous=NORMAL;") &&
           exec(db_, "PRAGMA temp_store=MEMORY;") &&
           // Bounded page cache keeps daemon RSS in budget (NFR4): ~8MB.
           exec(db_, "PRAGMA cache_size=-8000;");
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
        "  state INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS files_state ON files(state);"
        "CREATE INDEX IF NOT EXISTS files_parent ON files(parent);"
        // Extracted text, compressed. Only live (searchable) rows have a doc:
        // tombstoning deletes the doc, which via the triggers removes the FTS
        // postings — so the query path needs no state filter or join.
        "CREATE TABLE IF NOT EXISTS docs("
        "  id INTEGER PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  body_z BLOB);"
        "CREATE VIEW IF NOT EXISTS docs_v(id,name,body) AS"
        "  SELECT id, name, wl_unzstd(body_z) FROM docs;";
    if (!exec(db_, base)) return false;

    // The FTS virtual table reads original text through docs_v (this is what
    // makes snippet() work without storing uncompressed text, challenge §3).
    // prefix='3' backs the as-you-type worst case (content_min_query=3);
    // longer prefixes are selective enough for FTS5's term-range scan.
    std::string fts;
    if (match_ == MatchMode::Substring) {
        fts =
            "CREATE VIRTUAL TABLE IF NOT EXISTS files_fts USING fts5("
            "  name, body, content='docs_v', content_rowid='id',"
            "  tokenize='trigram');";
    } else {
        fts =
            "CREATE VIRTUAL TABLE IF NOT EXISTS files_fts USING fts5("
            "  name, body, content='docs_v', content_rowid='id',"
            "  tokenize='wl remove_diacritics 2', prefix='3');";
    }
    if (!exec(db_, fts.c_str())) return false;

    const char* aux =
        // Term → document-frequency access for the query planner (§8).
        "CREATE VIRTUAL TABLE IF NOT EXISTS files_fts_v USING fts5vocab('files_fts','row');"
        // Keep files_fts synchronized with docs. The 'delete' command must see
        // the exact text that was indexed; decompressing the stored frame is
        // deterministic, so it does.
        "CREATE TRIGGER IF NOT EXISTS docs_ai AFTER INSERT ON docs BEGIN"
        "  INSERT INTO files_fts(rowid,name,body)"
        "    VALUES(new.id,new.name,wl_unzstd(new.body_z));"
        "END;"
        "CREATE TRIGGER IF NOT EXISTS docs_ad AFTER DELETE ON docs BEGIN"
        "  INSERT INTO files_fts(files_fts,rowid,name,body)"
        "    VALUES('delete',old.id,old.name,wl_unzstd(old.body_z));"
        "END;"
        "CREATE TRIGGER IF NOT EXISTS docs_au AFTER UPDATE ON docs BEGIN"
        "  INSERT INTO files_fts(files_fts,rowid,name,body)"
        "    VALUES('delete',old.id,old.name,wl_unzstd(old.body_z));"
        "  INSERT INTO files_fts(rowid,name,body)"
        "    VALUES(new.id,new.name,wl_unzstd(new.body_z));"
        "END;";
    if (!exec(db_, aux)) return false;

    // Auto-merge segments during writes (our transient→static housekeeping).
    exec(db_, "INSERT INTO files_fts(files_fts,rank) VALUES('automerge',4);");

    std::string meta =
        "INSERT INTO meta(key,value) VALUES('match','" + std::string(mode_str(match_)) +
        "') ON CONFLICT(key) DO UPDATE SET value=excluded.value;"
        "INSERT INTO meta(key,value) VALUES('tokenizer','wl+unicode61+segments+zstd')"
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
        exec(db_, "DROP TRIGGER IF EXISTS docs_ai;"
                  "DROP TRIGGER IF EXISTS docs_ad;"
                  "DROP TRIGGER IF EXISTS docs_au;"
                  "DROP TABLE IF EXISTS files_fts_v;"
                  "DROP TABLE IF EXISTS files_fts;"
                  "DROP VIEW IF EXISTS docs_v;"
                  "DROP TABLE IF EXISTS docs;"
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
    opts_ = opts;
    avail_ = Availability::Error;

    if (opts.read_only) {
        // Prefer a strictly read-only handle: no WAL recovery, no -shm
        // creation, no side effects (challenge §5.1). SQLite ≥3.22 serves WAL
        // databases read-only by building a private in-heap wal-index when the
        // shared one is unavailable. Only if that fails, fall back to a
        // read-write handle pinned by PRAGMA query_only (availability rescue
        // when the WAL needs the shared-memory path, e.g. daemon running).
        std::error_code ec;
        if (!std::filesystem::exists(path_, ec)) {
            avail_ = Availability::NoIndex;
            return false;
        }
        if (!open_handle(SQLITE_OPEN_READONLY) &&
            !open_handle(SQLITE_OPEN_READWRITE)) {
            avail_ = Availability::Locked;   // exists but unopenable → likely lock/perm
            return false;
        }
        set_pragmas();

        if (scalar_int(db_, "SELECT count(*) FROM sqlite_master WHERE name='files_fts';") == 0) {
            close();
            avail_ = Availability::NoIndex;   // no schema yet → degrade to filenames
            return false;
        }
        if (scalar_int(db_, "PRAGMA user_version;") != kSchemaVersion) {
            close();
            avail_ = Availability::VersionMismatch;  // daemon will rebuild; degrade now
            return false;
        }
        // Adopt whatever mode the writer built.
        std::string stored = scalar_text(db_, "SELECT value FROM meta WHERE key='match';");
        match_ = (stored == "substring") ? MatchMode::Substring : MatchMode::Prefix;
        avail_ = Availability::Ok;
        return true;
    }

    // ---- writer ----
    // Ensure a 0700 data dir exists (NFR7).
    std::error_code ec;
    std::filesystem::path p(db_path);
    std::filesystem::create_directories(p.parent_path(), ec);
    if (!p.parent_path().empty()) ::chmod(p.parent_path().c_str(), 0700);

    if (!open_handle(SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)) return false;
    if (!set_pragmas()) { close(); return false; }

    // Self-heal: corruption or a schema/tokenizer version change both mean the
    // derived index is not trustworthy — start clean and let the crawl rebuild.
    std::string check = scalar_text(db_, "PRAGMA quick_check;");
    bool has_schema =
        scalar_int(db_, "SELECT count(*) FROM sqlite_master WHERE name='files';") > 0;
    bool stale_version =
        has_schema && scalar_int(db_, "PRAGMA user_version;") != kSchemaVersion;
    if ((check != "ok" && !check.empty()) || stale_version) {
        if (!recreate_db()) { close(); return false; }
    }

    if (!ensure_match_mode(opts.match)) { close(); return false; }

    // Lock down the DB file itself (NFR7).
    ::chmod(db_path.c_str(), 0600);
    avail_ = Availability::Ok;
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
    if (!exec(db_, "SAVEPOINT wlput;")) return false;

    bool ok = false;
    do {
        const char* sql =
            "INSERT INTO files(path,name,parent,size,mtime,mime,content_hash,indexed_at,state)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)"
            " ON CONFLICT(path) DO UPDATE SET"
            "   name=excluded.name, parent=excluded.parent, size=excluded.size,"
            "   mtime=excluded.mtime, mime=excluded.mime, content_hash=excluded.content_hash,"
            "   indexed_at=excluded.indexed_at, state=excluded.state;";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) break;
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
        sqlite3_bind_int64(st, 8, rec.indexed_at ? rec.indexed_at
                                                 : static_cast<int64_t>(::time(nullptr)));
        sqlite3_bind_int(st, 9, static_cast<int>(rec.state));
        bool step_ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!step_ok) break;

        sqlite3_stmt* idst = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT id FROM files WHERE path=?1;", -1, &idst,
                               nullptr) != SQLITE_OK)
            break;
        sqlite3_bind_text(idst, 1, rec.path.c_str(), -1, SQLITE_TRANSIENT);
        int64_t id = (sqlite3_step(idst) == SQLITE_ROW) ? sqlite3_column_int64(idst, 0) : 0;
        sqlite3_finalize(idst);
        if (id == 0) break;

        // Every live row gets a doc (empty body → filename-only searchability);
        // the docs triggers propagate to files_fts.
        std::string z = compress_body(body);
        sqlite3_stmt* dst = nullptr;
        const char* dsql =
            "INSERT INTO docs(id,name,body_z) VALUES(?1,?2,?3)"
            " ON CONFLICT(id) DO UPDATE SET name=excluded.name, body_z=excluded.body_z;";
        if (sqlite3_prepare_v2(db_, dsql, -1, &dst, nullptr) != SQLITE_OK) break;
        sqlite3_bind_int64(dst, 1, id);
        sqlite3_bind_text(dst, 2, rec.name.c_str(), -1, SQLITE_TRANSIENT);
        if (z.empty()) sqlite3_bind_null(dst, 3);
        else sqlite3_bind_blob64(dst, 3, z.data(), z.size(), SQLITE_TRANSIENT);
        ok = sqlite3_step(dst) == SQLITE_DONE;
        sqlite3_finalize(dst);
    } while (false);

    if (ok) {
        exec(db_, "RELEASE wlput;");
    } else {
        exec(db_, "ROLLBACK TO wlput;");
        exec(db_, "RELEASE wlput;");
    }
    return ok;
}

bool Store::remove(const std::string& path) {
    if (!db_ || read_only_) return false;
    if (!exec(db_, "SAVEPOINT wlrm;")) return false;
    bool ok = false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "DELETE FROM docs WHERE id IN (SELECT id FROM files WHERE path=?1);",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    sqlite3_finalize(st);
    int removed = 0;
    if (ok) {
        st = nullptr;
        ok = false;
        if (sqlite3_prepare_v2(db_, "DELETE FROM files WHERE path=?1;", -1, &st,
                               nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(st) == SQLITE_DONE;
            removed = sqlite3_changes(db_);
        }
        sqlite3_finalize(st);
    }
    if (ok) {
        exec(db_, "RELEASE wlrm;");
    } else {
        exec(db_, "ROLLBACK TO wlrm;");
        exec(db_, "RELEASE wlrm;");
    }
    return ok && removed > 0;
}

bool Store::remove_subtree(const std::string& path) {
    if (!db_ || read_only_ || path.empty() || path == "/") return false;
    std::string docs_sql = std::string("DELETE FROM docs WHERE id IN "
                                       "(SELECT id FROM files WHERE ") + kSubtreeWhere + ");";
    std::string files_sql = std::string("DELETE FROM files WHERE ") + kSubtreeWhere + ";";
    if (!exec(db_, "SAVEPOINT wlrmt;")) return false;
    bool ok = false;
    for (const std::string& sql : {docs_sql, files_sql}) {
        sqlite3_stmt* st = nullptr;
        ok = false;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(st) == SQLITE_DONE;
        }
        sqlite3_finalize(st);
        if (!ok) break;
    }
    if (ok) {
        exec(db_, "RELEASE wlrmt;");
    } else {
        exec(db_, "ROLLBACK TO wlrmt;");
        exec(db_, "RELEASE wlrmt;");
    }
    return ok;
}

bool Store::tombstone(const std::string& path) {
    if (!db_ || read_only_) return false;
    if (!exec(db_, "SAVEPOINT wlts;")) return false;
    bool ok = false;
    int changed = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE files SET state=2 WHERE path=?1;", -1, &st,
                           nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(st) == SQLITE_DONE;
        changed = sqlite3_changes(db_);
    }
    sqlite3_finalize(st);
    if (ok && changed > 0) {
        // Drop the doc now: the triggers remove the FTS postings, so a
        // tombstoned row is invisible to queries with no state filtering.
        st = nullptr;
        ok = false;
        if (sqlite3_prepare_v2(db_,
                "DELETE FROM docs WHERE id IN (SELECT id FROM files WHERE path=?1);",
                -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(st) == SQLITE_DONE;
        }
        sqlite3_finalize(st);
    }
    if (ok) {
        exec(db_, "RELEASE wlts;");
    } else {
        exec(db_, "ROLLBACK TO wlts;");
        exec(db_, "RELEASE wlts;");
    }
    return ok && changed > 0;
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
    exec(db_, "DELETE FROM docs;");
    exec(db_, "DELETE FROM files;");
    exec(db_, "INSERT INTO files_fts(files_fts) VALUES('rebuild');");
    return true;
}

bool Store::maintain() {
    if (!db_ || read_only_) return false;
    // Purge tombstones (their docs/FTS entries were dropped at tombstone time),
    // then merge segments.
    exec(db_, "DELETE FROM docs WHERE id IN (SELECT id FROM files WHERE state=2);");
    exec(db_, "DELETE FROM files WHERE state=2;");
    exec(db_, "INSERT INTO files_fts(files_fts,rank) VALUES('merge',16);");
    exec(db_, "PRAGMA wal_checkpoint(PASSIVE);");
    return true;
}

std::string Store::build_match_query(const std::string& user_query, bool prefix_last) const {
    // Turn free text into a safe FTS5 MATCH expression: quote each whitespace-
    // separated term (implicit AND). A quoted term that the tokenizer segments
    // (CamelCase/underscore/hyphen) compiles to a phrase, which matches the
    // segment positions the index stores — the two sides agree by construction
    // (challenge §1.3). With prefix_last the final term gets a trailing '*'
    // (phrase-prefix: the last segment completes as-you-type); in substring
    // mode terms shorter than a trigram can't match, so drop them.
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
    if (prefix_last && match_ == MatchMode::Prefix) terms.back() += "*";
    std::string out;
    for (size_t i = 0; i < terms.size(); i++) {
        if (i) out += ' ';
        out += terms[i];
    }
    return out;
}

bool Store::query_is_common(const std::string& user_query) const {
    // Cheap selectivity estimate via fts5vocab: the cost of a ranked AND query
    // tracks the *smallest* document frequency among its terms. Unknown terms
    // (segmented compounds, absent vocab table) count as selective — the
    // query-budget interrupt still bounds the miss.
    if (opts_.common_term_df <= 0) return false;
    sqlite3_stmt* exact = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT doc FROM files_fts_v WHERE term=?1;",
                           -1, &exact, nullptr) != SQLITE_OK)
        return false;

    int64_t min_df = -1;
    std::istringstream iss(user_query);
    std::string tok;
    bool any = false;
    while (iss >> tok) {
        std::string t;
        for (char c : tok)
            t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (t.empty()) continue;
        any = true;
        sqlite3_reset(exact);
        sqlite3_bind_text(exact, 1, t.c_str(), -1, SQLITE_TRANSIENT);
        int64_t df = 0;
        if (sqlite3_step(exact) == SQLITE_ROW) df = sqlite3_column_int64(exact, 0);
        if (min_df < 0 || df < min_df) min_df = df;
        if (min_df == 0) break;   // an unknown/rare term makes the query selective
    }
    sqlite3_finalize(exact);
    return any && min_df > opts_.common_term_df;
}

bool Store::ranked_rowids_full(const std::string& match, int limit,
                               std::vector<std::pair<int64_t, double>>& out) {
    // True global BM25 top-K. No join, no snippet: tombstones have no postings
    // and snippets are hydrated later for the winners only.
    const char* sql =
        "SELECT files_fts.rowid, bm25(files_fts,10.0,1.0) AS s"
        " FROM files_fts WHERE files_fts MATCH ?1 ORDER BY s LIMIT ?2;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        avail_ = Availability::Error;
        return true;   // hard failure: bounded retry would fail identically
    }
    sqlite3_bind_text(st, 1, match.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, limit);

    int64_t deadline = now_ms() + opts_.query_budget_ms;
    if (opts_.query_budget_ms > 0)
        sqlite3_progress_handler(db_, 4096, deadline_cb, &deadline);

    bool interrupted = false;
    for (;;) {
        int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            out.emplace_back(sqlite3_column_int64(st, 0), sqlite3_column_double(st, 1));
        } else {
            interrupted = (rc == SQLITE_INTERRUPT);
            break;
        }
    }
    if (opts_.query_budget_ms > 0)
        sqlite3_progress_handler(db_, 0, nullptr, nullptr);
    sqlite3_finalize(st);
    if (interrupted) out.clear();
    return !interrupted;
}

void Store::ranked_rowids_bounded(const std::string& match, int limit,
                                  std::vector<std::pair<int64_t, double>>& out) {
    // Bounded-work fallback for broad queries: walk the doclist newest-first
    // (FTS5 streams MATCH in rowid order natively — no sort), score only the
    // candidate budget, and rank within it. Trades global BM25 optimality for
    // a latency ceiling independent of corpus size (challenge §8.3).
    out.clear();
    int budget = std::max(limit, opts_.candidate_budget);
    const char* sql =
        "SELECT files_fts.rowid, bm25(files_fts,10.0,1.0) AS s"
        " FROM files_fts WHERE files_fts MATCH ?1"
        " ORDER BY files_fts.rowid DESC LIMIT ?2;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        avail_ = Availability::Error;
        return;
    }
    sqlite3_bind_text(st, 1, match.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, budget);
    while (sqlite3_step(st) == SQLITE_ROW)
        out.emplace_back(sqlite3_column_int64(st, 0), sqlite3_column_double(st, 1));
    sqlite3_finalize(st);

    size_t keep = static_cast<size_t>(limit);
    if (out.size() > keep) {
        std::partial_sort(out.begin(), out.begin() + keep, out.end(),
                          [](const auto& a, const auto& b) { return a.second < b.second; });
        out.resize(keep);
    } else {
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });
    }
}

std::vector<ContentHit> Store::search(const std::string& query, int limit,
                                      const std::string& hl_open, const std::string& hl_close) {
    std::vector<ContentHit> hits;
    if (!db_ || limit <= 0) return hits;
    static const bool trace = ::getenv("WL_SEARCH_TRACE") != nullptr;
    int64_t t0 = now_ms();

    // Phase 1: ranked rowids only. The planner picks between a full global-BM25
    // top-K (selective queries — sub-millisecond) and a bounded exact-term scan
    // (ultra-common queries — avoids the O(posting-list) worst case, §8). Each
    // path uses a different MATCH form, and phase 2 must reuse the same one so
    // snippet() highlights what phase 1 actually matched.
    std::string match;
    std::vector<std::pair<int64_t, double>> top;
    bool common = query_is_common(query);
    int64_t t1 = now_ms();
    bool bounded = common;
    if (!common) {
        match = build_match_query(query, /*prefix_last=*/true);
        if (match.empty()) return hits;
        if (!ranked_rowids_full(match, limit, top)) bounded = true;
    }
    if (bounded) {
        match = build_match_query(query, /*prefix_last=*/false);
        if (match.empty()) return hits;
        ranked_rowids_bounded(match, limit, top);
    }
    int64_t t2 = now_ms();

    if (top.empty()) {
        if (trace)
            std::fprintf(stderr, "[wl] q=%-16s classify=%s plan=%s df=%lldms plan=%lldms hits=0\n",
                         query.c_str(), common ? "common" : "sel",
                         bounded ? "bounded" : "full", (long long)(t1 - t0),
                         (long long)(t2 - t1));
        return hits;
    }

    // Phase 2: hydrate metadata + snippet for the winners only. snippet()
    // decompresses the body through docs_v, so it runs ≤ limit times total —
    // never per candidate (this was the hidden O(matches) cost before).
    const char* sql =
        "SELECT f.path, f.name, f.mime,"
        "       snippet(files_fts,1,?2,?3,'…',12) AS snip"
        " FROM files_fts JOIN files f ON f.id=files_fts.rowid"
        " WHERE files_fts MATCH ?1 AND files_fts.rowid=?4;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        avail_ = Availability::Error;
        return hits;
    }
    for (const auto& [rowid, score] : top) {
        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, match.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, hl_open.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, hl_close.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, rowid);
        if (sqlite3_step(st) != SQLITE_ROW) continue;   // row vanished mid-flight
        ContentHit h;
        if (const unsigned char* p = sqlite3_column_text(st, 0))
            h.path.assign(reinterpret_cast<const char*>(p));
        if (const unsigned char* n = sqlite3_column_text(st, 1))
            h.name.assign(reinterpret_cast<const char*>(n));
        if (const unsigned char* m = sqlite3_column_text(st, 2))
            h.mime.assign(reinterpret_cast<const char*>(m));
        if (const unsigned char* s = sqlite3_column_text(st, 3))
            h.snippet.assign(reinterpret_cast<const char*>(s));
        h.score = -score;   // negate: SQLite bm25 is lower-is-better
        hits.push_back(std::move(h));
    }
    sqlite3_finalize(st);
    if (trace)
        std::fprintf(stderr,
                     "[wl] q=%-16s classify=%s plan=%s df=%lldms plan1=%lldms snip=%lldms hits=%zu\n",
                     query.c_str(), common ? "common" : "sel", bounded ? "bounded" : "full",
                     (long long)(t1 - t0), (long long)(t2 - t1),
                     (long long)(now_ms() - t2), hits.size());
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
