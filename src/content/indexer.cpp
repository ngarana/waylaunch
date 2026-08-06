#include "waylaunch/content/indexer.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace waylaunch::content {

namespace {

constexpr int kBatch = 512;          // files per write transaction
constexpr int kMaintainEvery = 20;   // batches between housekeeping passes

int64_t stat_mtime_ns(const struct stat& sb) {
    return static_cast<int64_t>(sb.st_mtim.tv_sec) * 1000000000LL + sb.st_mtim.tv_nsec;
}

// FNV-1a 64 over up to `cap` bytes of a file — cheap change confirmation (FR7).
std::string hash_file(const std::string& path, size_t cap) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    uint64_t h = 1469598103934665603ULL;
    std::array<char, 65536> buf;
    size_t total = 0;
    while (f && total < cap) {
        f.read(buf.data(), static_cast<std::streamsize>(
                               std::min(buf.size(), cap - total)));
        std::streamsize n = f.gcount();
        for (std::streamsize i = 0; i < n; i++) {
            h ^= static_cast<unsigned char>(buf[i]);
            h *= 1099511628211ULL;
        }
        total += static_cast<size_t>(n);
        if (n == 0) break;
    }
    std::string out(8, '\0');
    for (int i = 0; i < 8; i++) out[i] = static_cast<char>((h >> (i * 8)) & 0xff);
    return out;
}

bool has_prefix(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    if (s.compare(0, prefix.size(), prefix) != 0) return false;
    // boundary: exact, or next char is '/'
    return s.size() == prefix.size() || s[prefix.size()] == '/';
}

// Is this the name of a content-addressed blob rather than a document?
//
// Object stores name files after a digest of their contents: pnpm's store
// (126 hex chars), git objects, nix, ccache, PostgreSQL WAL segments
// ("000000010000004000000019"), pre-commit's cache ("<md5>.resolved2"). No
// exclude list can enumerate them all — new tools keep inventing stores — but
// they share a giveaway: a stem that is one long run of hex digits. Such a name
// is useless to search for, and the contents are packed/binary payloads, so
// indexing them is pure cost. This is what filled a real index with rows like
// "862a8eb308e285bc89683ce9378354e0…" and pushed junk into the CONTENTS list.
//
// The 20-digit floor keeps ordinary hex-ish names (a "deadbeef.log", an 8-digit
// build id) indexable; a real document with a 20+ hex-digit stem is vanishingly
// rare, and it stays findable by path//filename search regardless.
bool looks_like_hash_name(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = base.find('.');
    std::string stem = (dot == std::string::npos) ? base : base.substr(0, dot);
    if (stem.size() < 20) return false;
    for (unsigned char c : stem)
        if (!std::isxdigit(c)) return false;
    return true;
}

// Best-effort idle I/O priority for the calling thread (NFR3). Mirrors the
// systemd unit's IOSchedulingClass=idle for from-source runs without systemd.
void set_ionice_idle() {
    constexpr int IOPRIO_WHO_PROCESS = 1;
    constexpr int IOPRIO_CLASS_IDLE = 3;
    syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_CLASS_IDLE << 13);
}

// True when running on battery (any AC "Mains" supply reporting offline).
bool on_battery() {
    std::error_code ec;
    fs::path base("/sys/class/power_supply");
    if (!fs::exists(base, ec)) return false;
    for (fs::directory_iterator it(base, ec), end; it != end; it.increment(ec)) {
        std::ifstream tf(it->path() / "type");
        std::string type;
        tf >> type;
        if (type == "Mains") {
            std::ifstream of(it->path() / "online");
            int online = 1;
            of >> online;
            if (!online) return true;
        }
    }
    return false;
}

} // namespace

Indexer::Indexer(Store& store, ContentConfig cfg)
    : store_(store),
      cfg_(std::move(cfg)),
      extractor_(cfg_.extractors),
      max_file_bytes_(cfg_.max_file_mb * 1024 * 1024) {}

Indexer::~Indexer() { stop(); }

bool Indexer::under_roots(const std::string& path) const {
    for (const auto& r : cfg_.roots)
        if (has_prefix(path, r)) return true;
    return false;
}

bool Indexer::is_excluded(const std::string& path) const {
    // Content-addressed blobs (object stores, WAL segments) — see the helper.
    if (looks_like_hash_name(path)) return true;
    // Privacy prefixes: never read.
    for (const auto& p : cfg_.exclude_paths)
        if (has_prefix(path, p)) return true;
    // Runtime excludes added via the control channel (FR8).
    {
        std::lock_guard<std::mutex> lk(excl_mtx_);
        for (const auto& p : runtime_excludes_)
            if (has_prefix(path, p)) return true;
    }
    // Noise excludes: a bare name matches any path component; a pattern with '/'
    // matches as a path substring (e.g. "go/pkg").
    for (const auto& e : cfg_.excludes) {
        if (e.empty()) continue;
        if (e.find('/') != std::string::npos) {
            if (path.find(e) != std::string::npos) return true;
        } else {
            // component match
            size_t pos = 0;
            while ((pos = path.find(e, pos)) != std::string::npos) {
                bool left = (pos == 0) || path[pos - 1] == '/';
                size_t end = pos + e.size();
                bool right = (end == path.size()) || path[end] == '/';
                if (left && right) return true;
                pos = end;
            }
        }
    }
    return false;
}

void Indexer::index_file(const std::string& path) {
    struct stat sb;
    if (::stat(path.c_str(), &sb) != 0) {   // vanished
        store_.remove(path);
        return;
    }
    if (!S_ISREG(sb.st_mode)) return;       // dirs walked separately; skip fifos/etc.
    if (is_excluded(path)) { store_.remove(path); return; }

    int64_t mtime = stat_mtime_ns(sb);
    int64_t size = static_cast<int64_t>(sb.st_size);
    auto existing = store_.get(path);
    if (existing && existing->state == FileState::Indexed &&
        existing->mtime_ns == mtime && existing->size == size)
        return;   // unchanged

    std::string hash = hash_file(path, max_file_bytes_);
    if (existing && !existing->content_hash.empty() && existing->content_hash == hash) {
        store_.touch(path, mtime, size);   // touched but identical (FR7)
        return;
    }

    fs::path fp(path);
    FileRecord rec;
    rec.path = path;
    rec.name = fp.filename().string();
    rec.parent = fp.parent_path().string();
    rec.size = size;
    rec.mtime_ns = mtime;
    rec.content_hash = hash;
    rec.state = FileState::Indexed;

    std::string body;
    if (size > static_cast<int64_t>(max_file_bytes_)) {
        // Too large to extract: index metadata only (filename stays searchable).
        rec.mime = detect_mime(path);
    } else {
        ExtractResult r = extractor_.extract(path, cfg_.extract_options());
        rec.mime = r.mime;
        if (r.status == ExtractStatus::Ok || r.status == ExtractStatus::Empty ||
            r.status == ExtractStatus::Unsupported) {
            body = std::move(r.text);
        } else {   // Timeout / Error: keep metadata, mark error, remember hash so
                   // we don't re-attempt until the file changes (NFR6).
            rec.state = FileState::Error;
            errors_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (store_.put(rec, body) && rec.state == FileState::Indexed)
        indexed_.fetch_add(1, std::memory_order_relaxed);
}

bool Indexer::size_cap_reached(int64_t& used_out) {
    const int64_t cap = static_cast<int64_t>(cfg_.max_index_mb) * 1024 * 1024;
    StoreStats st = store_.stats();
    used_out = st.db_used_bytes();
    db_used_bytes_.store(used_out, std::memory_order_relaxed);
    if (cap <= 0) return false;                 // unlimited
    if (used_out <= cap) return false;

    // Over cap on live content. If the file is bloated with free pages, a
    // VACUUM may be all that is needed — but it rewrites the entire database,
    // so only pay for it when the freelist is actually worth reclaiming.
    constexpr int64_t kVacuumWorthwhile = 64ll * 1024 * 1024;
    if (st.db_free_bytes >= kVacuumWorthwhile) {
        std::fprintf(stderr, "waylaunchd: index over cap with %lld MB of free pages; "
                     "compacting\n", static_cast<long long>(st.db_free_bytes / (1024 * 1024)));
        store_.vacuum();
        st = store_.stats();
        used_out = st.db_used_bytes();
        db_used_bytes_.store(used_out, std::memory_order_relaxed);
        if (used_out <= cap) return false;
    }
    return true;
}

void Indexer::crawl_all() {
    crawling_.store(true, std::memory_order_relaxed);
    const int64_t size_cap = static_cast<int64_t>(cfg_.max_index_mb) * 1024 * 1024;

    // Check the cap BEFORE walking anything. The crawl is re-run on every
    // reconcile, and extraction is the expensive part: an index already at the
    // cap used to re-extract a whole batch each cycle only to abort at the first
    // commit, which is the churn (and the repeated log line) this avoids. State
    // is latched so the message is printed on transition, not every interval.
    {
        int64_t used = 0;
        if (size_cap_reached(used)) {
            if (!size_capped_.exchange(true, std::memory_order_relaxed)) {
                std::fprintf(stderr,
                             "waylaunchd: index size cap reached (%lld MB used of %zu MB cap); "
                             "no new files will be indexed. Existing entries are still "
                             "refreshed and deletions still reclaim space. Raise "
                             "[content] max_index_mb or narrow [content] roots/excludes.\n",
                             static_cast<long long>(used / (1024 * 1024)), cfg_.max_index_mb);
            }
            crawling_.store(false, std::memory_order_relaxed);
            return;
        }
        if (size_capped_.exchange(false, std::memory_order_relaxed))
            std::fprintf(stderr, "waylaunchd: index back under cap (%lld MB); resuming crawl\n",
                         static_cast<long long>(used / (1024 * 1024)));
    }

    int since_commit = 0, since_throttle = 0;
    bool capped = false;
    store_.begin();
    for (const auto& root : cfg_.roots) {
        if (capped) break;
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;
        auto opts = fs::directory_options::skip_permission_denied;
        fs::recursive_directory_iterator it(root, opts, ec), end;
        for (; it != end; it.increment(ec)) {
            if (stop_.load(std::memory_order_relaxed)) break;
            wait_if_paused();
            if (ec) { ec.clear(); continue; }
            const fs::path& p = it->path();
            std::string sp = p.string();
            // Prune excluded/privacy directories (don't descend).
            std::error_code dec;
            if (it->is_directory(dec)) {
                if (is_excluded(sp)) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(dec)) continue;
            if (is_excluded(sp)) continue;
            index_file(sp);
            // Adaptive throttle: yield periodically on battery (NFR3 / §7).
            if (cfg_.throttle_on_battery && ++since_throttle >= 64) {
                since_throttle = 0;
                if (on_battery()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (++since_commit >= kBatch) {
                store_.commit();
                int64_t used = 0;
                if (size_cap > 0 && size_cap_reached(used)) {
                    if (!size_capped_.exchange(true, std::memory_order_relaxed))
                        std::fprintf(stderr,
                                     "waylaunchd: index size cap reached mid-crawl "
                                     "(%lld MB used of %zu MB cap); stopping discovery\n",
                                     static_cast<long long>(used / (1024 * 1024)),
                                     cfg_.max_index_mb);
                    capped = true;
                    store_.begin();   // reopen a txn for the final commit below
                    break;
                }
                store_.begin();
                since_commit = 0;
            }
        }
        if (stop_.load(std::memory_order_relaxed)) break;
    }
    store_.commit();
    crawling_.store(false, std::memory_order_relaxed);
}

void Indexer::reconcile_deletions() {
    // Walk the index; remove rows whose file is gone or now excluded (FR4).
    std::vector<std::string> gone;
    store_.for_each_indexed_path([&](const std::string& path) {
        struct stat sb;
        if (::stat(path.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode) || is_excluded(path))
            gone.push_back(path);
    });
    if (gone.empty()) return;
    store_.begin();
    for (const auto& p : gone) store_.remove(p);
    store_.commit();
}

void Indexer::wait_if_paused() {
    if (!paused_.load(std::memory_order_relaxed)) return;
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [&] { return !paused_.load() || stop_.load(); });
}

void Indexer::full_reconcile() {
    // The full freshness pass: re-crawl (adds/modifies; unchanged files are
    // skipped cheaply by the mtime/size gate in index_file) and reconcile
    // deletions, then merge. This is both the startup pass (FR4) and the
    // periodic backstop for anything inotify missed (§4.5/§9).
    crawl_all();
    reconcile_deletions();
    store_.maintain();
    last_reconcile_s_.store(static_cast<int64_t>(::time(nullptr)), std::memory_order_relaxed);
}

int Indexer::reconcile_interval() const {
    if (watch_degraded_.load(std::memory_order_relaxed) &&
        cfg_.reconcile_interval_degraded_s > 0)
        return cfg_.reconcile_interval_degraded_s;
    return cfg_.reconcile_interval_s;
}

void Indexer::run_loop() {
    setpriority(PRIO_PROCESS, 0, cfg_.worker_nice);   // background CPU priority (NFR3)
    set_ionice_idle();

    using clock = std::chrono::steady_clock;
    if (reindex_.exchange(false)) store_.clear();
    full_reconcile();
    clock::time_point last_pass = clock::now();

    int batches = 0;
    while (!stop_.load()) {
        std::deque<WorkItem> batch;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            auto pred = [&] {
                return stop_.load() || reconcile_.load() || reindex_.load() ||
                       !queue_.empty();
            };
            // Recompute the deadline each iteration so a mid-wait switch to the
            // degraded (shorter) interval takes effect on the next wake.
            int iv = reconcile_interval();
            if (iv > 0)
                cv_.wait_until(lk, last_pass + std::chrono::seconds(iv), pred);
            else
                cv_.wait(lk, pred);
            if (stop_.load()) break;

            if (reindex_.exchange(false)) {
                lk.unlock();
                store_.clear();
                full_reconcile();
                last_pass = clock::now();
                continue;
            }
            // Explicit reconcile (overflow/control) or the periodic deadline —
            // both run the same full pass. The deadline reset prevents a
            // busy queue from starving or re-triggering the backstop.
            bool periodic_due = iv > 0 && clock::now() >= last_pass + std::chrono::seconds(iv);
            if (reconcile_.exchange(false) || periodic_due) {
                lk.unlock();
                full_reconcile();
                last_pass = clock::now();
                continue;
            }
            // take a bounded batch
            while (!queue_.empty() && batch.size() < kBatch) {
                batch.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
        }
        wait_if_paused();
        if (stop_.load()) break;

        store_.begin();
        for (const auto& item : batch) {
            if (item.kind == WorkItem::Remove) store_.remove(item.path);
            else if (item.kind == WorkItem::RemoveTree) store_.remove_subtree(item.path);
            else index_file(item.path);
        }
        store_.commit();

        if (++batches % kMaintainEvery == 0) store_.maintain();
    }
    store_.maintain();
}

void Indexer::run_once() {
    setpriority(PRIO_PROCESS, 0, cfg_.worker_nice);
    set_ionice_idle();
    if (reindex_.exchange(false)) store_.clear();
    full_reconcile();
}

void Indexer::start() {
    if (thread_.joinable()) return;
    stop_.store(false);
    thread_ = std::thread([this] { run_loop(); });
}

void Indexer::stop() {
    if (!thread_.joinable()) return;
    stop_.store(true);
    cv_.notify_all();
    thread_.join();
}

void Indexer::enqueue_index(std::string path) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.push_back({WorkItem::Index, std::move(path)});
    }
    cv_.notify_one();
}

void Indexer::enqueue_remove(std::string path) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.push_back({WorkItem::Remove, std::move(path)});
    }
    cv_.notify_one();
}

void Indexer::enqueue_remove_tree(std::string path) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.push_back({WorkItem::RemoveTree, std::move(path)});
    }
    cv_.notify_one();
}

void Indexer::request_reconcile() {
    reconcile_.store(true);
    cv_.notify_one();
}

void Indexer::request_reindex() {
    reindex_.store(true);
    watch_degraded_.store(false, std::memory_order_relaxed);  // a fresh index re-adds watches
    cv_.notify_one();
}

void Indexer::set_watch_degraded(bool degraded) {
    bool was = watch_degraded_.exchange(degraded, std::memory_order_relaxed);
    // On first degrade: reconcile now (some subtrees just went unwatched), then
    // the loop switches to the shorter interval for subsequent passes.
    if (degraded && !was) request_reconcile();
}

void Indexer::add_runtime_exclude(std::string path) {
    {
        std::lock_guard<std::mutex> lk(excl_mtx_);
        runtime_excludes_.push_back(std::move(path));
    }
    request_reconcile();   // drop any now-excluded files already indexed
}

void Indexer::set_paused(bool paused) {
    paused_.store(paused);
    cv_.notify_all();
}

Indexer::Snapshot Indexer::snapshot() const {
    Snapshot s;
    s.indexed = indexed_.load(std::memory_order_relaxed);
    s.errors = errors_.load(std::memory_order_relaxed);
    s.crawling = crawling_.load(std::memory_order_relaxed);
    s.paused = paused_.load(std::memory_order_relaxed);
    s.watch_degraded = watch_degraded_.load(std::memory_order_relaxed);
    s.size_capped = size_capped_.load(std::memory_order_relaxed);
    s.db_used_bytes = db_used_bytes_.load(std::memory_order_relaxed);
    s.size_cap_bytes = static_cast<int64_t>(cfg_.max_index_mb) * 1024 * 1024;
    s.reconcile_interval_s = reconcile_interval();
    int64_t last = last_reconcile_s_.load(std::memory_order_relaxed);
    s.last_reconcile_ago_s = last ? (static_cast<int64_t>(::time(nullptr)) - last) : -1;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        s.queued = static_cast<int64_t>(queue_.size());
    }
    return s;
}

} // namespace waylaunch::content
