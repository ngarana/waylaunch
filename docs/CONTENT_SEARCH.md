# waylaunch — Content Search: Requirements & Design

**Status:** Proposal · **Author:** waylaunch · **Last updated:** 2026-07-19

Index-backed, "instant" full-text content search for waylaunch — the Spotlight
behaviour where typing `quarterly revenue` finds the *contents* of a spreadsheet,
not just files named like the query. This document specifies what to build and
why, grounded in how macOS Spotlight actually does it.

> **Why not just run `ripgrep` live?** We evaluated and rejected that. Grepping
> file *bytes* under `~` on every keystroke reads every file every time — it is
> O(corpus) per query, unbounded in latency, and violates waylaunch's
> minimal-resources goal. Real Spotlight is O(index-lookup) per query because the
> corpus is read **once**, at index time, and queries hit a prebuilt inverted
> index. This design does the same.

---

## 1. How macOS Spotlight actually works (researched)

Spotlight cleanly separates *indexing* (expensive, background, incremental) from
*querying* (cheap, instant, on-demand). The pieces:

| Component | Role |
|---|---|
| **`mds`** (metadata server) | Central daemon. Owns the stores, coordinates indexing, subscribes to change events. One authority per system. |
| **`mds_stores`** | Manages the on-disk index: compresses extracted content, merges transient data into the static store during housekeeping. |
| **`mdworker`** | Ephemeral, sandboxed worker processes. Each reads *one* file, picks the right importer, extracts text + metadata. Spawned in parallel (≈11 workers for 9 files), each alive ~0.2 s. Crash-isolated from `mds`. |
| **`mdimporter`** | Per-type plugin bundles (`RichText`, `PDF`, `Office`, `iWork`, `Image`…) selected by the file's UTI. Encapsulate "how to get text out of this format." |
| **FSEvents** | Filesystem change notification. Each volume's `.fseventsd` records create/modify/delete; Spotlight reindexes just the affected files, usually within 1–7 s. |
| **The store** (`.Spotlight-V100/Store-V2`, ~99 files) | The index itself. |
| **MDQuery / `NSMetadataQuery` / `mdfind`** | The query API. Predicates like `kMDItemTextContent CONTAINS[cd] "…"`, scoped to home/local/etc. |

**Index internals** (from Apple patents + reverse-engineering):

- It's an **inverted index**: a *dictionary* of tokens, each with a *posting
  list* of the documents/locations where the token occurs.
- **Separate** dictionaries/posting lists for **metadata** vs **content**.
- **Two-level design:** a small "live"/transient table optimised for fast
  *updates* (recent changes, frequent tokens) plus a large static table
  optimised for *search*. Periodic housekeeping merges transient → static.
- **Deletions** are tombstoned and purged during maintenance, not applied inline.
- **Tokenization** at Unicode word boundaries; `one target two`,
  `one_target_two`, `one-target-two`, and `OneTargetTwo` all tokenize to the same
  three words.
- Indexing is **throttled / low-priority** and heavy extraction (e.g. image OCR
  via `mediaanalysisd`) is deferred and slower than text extraction.

**The five properties we must reproduce:**

1. Read each file's content **once** (at index time), never per query.
2. Keep the index **fresh** via incremental change notification, not rescans.
3. Isolate extraction so a malformed file can't take down the indexer.
4. Rank metadata (filename) above content, surface content as its own section.
5. Stay **cheap**: background, throttled, bounded, resumable.

---

## 2. Goals & non-goals

### Goals
- Full-text search over the *contents* of user documents, returning results in
  **≤ ~30 ms** for typical queries regardless of corpus size.
- Freshness: a changed/created/deleted file reflected in results within seconds.
- Pluggable per-format text extraction (text, code, PDF, Office/ODF, HTML…).
- Strict resource envelope; safe to run continuously on a laptop.
- Integrate into the existing unified Spotlight UI as a distinct result group
  with a highlighted snippet — no modes.

### Non-goals (initial)
- Semantic / vector / OCR / image-content search (future; Spotlight's
  `mediaanalysisd` equivalent).
- Indexing removable/network volumes, or system/binary files.
- A general metadata-attribute query language (`kMDItem*` predicates). We index
  content + a few core attributes; a richer attribute store is future work.
- Multi-user / system-wide index. waylaunch indexes the **invoking user's**
  configured roots only.

---

## 3. Requirements

### Functional (FR)
- **FR1** Build and maintain a persistent full-text index over configured roots
  (default `~`), honouring an exclude list and a privacy exclude list.
- **FR2** Extract text via format-specific extractors chosen by MIME/extension;
  unknown/binary types are skipped (metadata-only).
- **FR3** Detect create/modify/delete/rename incrementally (no full rescan in the
  steady state) and update the index accordingly.
- **FR4** On startup, reconcile the index against the filesystem (catch changes
  missed while the daemon was down or events were dropped).
- **FR5** Answer content queries ranked by relevance, returning path, score, and
  a highlighted snippet with match offsets.
- **FR6** Surface content results in the launcher as a dedicated group,
  gated by `enable_content` and a minimum query length, ranked below filename/app
  hits (Spotlight ordering).
- **FR7** Never re-extract unchanged files (detect via mtime+size, verify via
  content hash).
- **FR8** Provide a control channel: index status, pause/resume, force reindex,
  and per-path exclusion at runtime.

### Non-functional (NFR)
- **NFR1 Query latency:** p50 ≤ 15 ms at 1 M indexed documents; p99 ≤ 50 ms for
  *bounded* queries, measured from query submit to ranked results (excludes UI
  paint). p99 is qualified because a strict global-BM25 top-K over an
  ultra-common single term is inherently O(posting-list) and grows with corpus
  size (see CONTENT_SEARCH_STORE.md §8). The store meets p99 by classifying such
  queries via document frequency and answering them with a bounded exact-term
  scan (§6.1 below); selective queries — the p50 population — stay sub-4 ms at
  1 M. Measured (`content_bench`, 1 M docs): selective p99 3.4 ms; ultra-common
  single term 44 ms with the planner vs 779 ms without.
- **NFR2 Freshness:** single-file edit reflected in query results ≤ 5 s under
  normal load (inotify path). In a subtree left unwatched because watch
  descriptors are exhausted, freshness instead is bounded by the periodic
  reconcile (`reconcile_interval_degraded_s`, default 3 min) — degraded but not
  lost; the fix is raising `fs.inotify.max_user_watches`.
- **NFR3 Indexer CPU:** background indexing runs at low priority (`nice ≥ 10`,
  best-effort `ionice`), single-digit % CPU steady-state; a full initial crawl
  may burst but must yield under load / on battery.
- **NFR4 Memory:** daemon steady-state RSS ≤ ~60 MB (bounded SQLite page cache +
  bounded work queues). Extraction workers are short-lived subprocesses.
- **NFR5 Disk:** a *snippet-capable* FTS5 index is ~150–250 % of indexed text
  volume, not the ≤15–25 % originally targeted — the 15–25 % figure assumed a
  compact posting-only index, but snippets, positions (for phrase/CamelCase
  matching), and prefix terms each cost space and cannot be dropped without
  losing a feature (see CONTENT_SEARCH_STORE.md §7). The store minimizes within
  that reality: extracted body text is **zstd-compressed** (~3× on prose) behind
  an external-content view, a single `prefix='3'` index (not `'2 3'`) matches the
  3-char `content_min_query` without indexing shorter prefixes, and per-file text
  is capped (default 2 MB). Measured (`content_bench`): ~203 % at 100 k docs,
  ~243 % at 1 M. A configurable hard `max_index_mb` cap stops the crawl if
  exceeded.
- **NFR6 Robustness:** an extractor crash/hang/OOM never crashes the daemon and
  never blocks queries; a corrupt index self-heals by rebuild.
- **NFR7 Privacy:** index stored under `$XDG_DATA_HOME` with `0700`/`0600` perms;
  no network access; excluded paths never read.
- **NFR8 Availability:** the launcher queries **read-only** and works whether or
  not the daemon is running (stale-but-usable index; degrades to filename search
  if no index exists).

---

## 4. Architecture

Mirror Spotlight's daemon/worker/store/client split.

```
                        ┌──────────────────────────────────────────┐
                        │            waylaunchd (daemon)            │   ≈ mds + mds_stores
                        │                                           │
   inotify/fanotify ───►│  FsWatcher ──► ChangeQueue ──► Scheduler  │
   (FSEvents analog)    │                                   │       │
                        │                                   ▼       │
                        │                          ExtractorPool    │   ≈ mdworker
                        │                        (subprocess workers)│
                        │                                   │       │
                        │                          Importer plugins │   ≈ mdimporter
                        │                          (text/pdf/office) │
                        │                                   │       │
                        │                                   ▼       │
                        │                            IndexWriter ───┼──► SQLite FTS5 store
                        │                                           │    (.../index.db, WAL)
                        │  ControlSocket (status/pause/reindex) ◄───┼──── waylaunchctl
                        └───────────────────────────────────────────┘
                                              ▲
                                              │  read-only (WAL reader)
                        ┌─────────────────────┴─────────────────────┐
                        │      waylaunch (launcher / UI client)      │   ≈ Spotlight UI + mdfind
                        │  ContentSearchProvider ──► SELECT … MATCH  │
                        └────────────────────────────────────────────┘
```

### 4.1 Component → Spotlight mapping

| waylaunch | Spotlight equivalent | Notes |
|---|---|---|
| `waylaunchd` | `mds` + `mds_stores` | Long-lived per-user daemon; owns the store. |
| `FsWatcher` | FSEvents subscription | inotify now; fanotify-capable abstraction. |
| `ExtractorPool` + `Importer` | `mdworker` + `mdimporter` | Subprocess isolation, per-type extractors. |
| SQLite **FTS5** store | `.Spotlight-V100` inverted index | Dictionary + posting lists + BM25, provided by FTS5. |
| `ContentSearchProvider` (in launcher) | `MDQuery`/`mdfind` | Read-only query, merged into unified results. |
| `waylaunchctl` / control socket | `mdutil` | Enable/disable/reindex/status. |

### 4.2 Why the daemon is separate from the launcher
- The launcher must be **instant**: it should never do indexing work at query
  time. Querying is a read-only `SELECT`.
- Indexing must proceed **while the launcher is closed** (waylaunch is invoked
  on-demand and exits). A persistent daemon keeps the index warm.
- Crash isolation: extraction bugs live in the daemon/workers, not the UI.
- Concurrency: SQLite in **WAL mode** allows one writer (daemon) + many readers
  (launcher) simultaneously with no IPC for the query path — the launcher opens
  the DB file directly, read-only. The control socket is only for management.

### 4.3 The store — schema

SQLite with FTS5. FTS5 *is* an inverted index (dictionary + posting lists) with
BM25 ranking and incremental segment merging — the direct analog of Spotlight's
two-level, transient→static store, but battle-tested and embeddable.

```sql
PRAGMA journal_mode = WAL;          -- concurrent readers + 1 writer
PRAGMA synchronous  = NORMAL;

-- Canonical file record (metadata; the "kMDItem*" analog, minimal). No body text
-- here, so metadata scans never drag body pages through the page cache.
CREATE TABLE files (
    id            INTEGER PRIMARY KEY,
    path          TEXT UNIQUE NOT NULL,   -- absolute
    name          TEXT NOT NULL,
    parent        TEXT NOT NULL,
    size          INTEGER NOT NULL,
    mtime         INTEGER NOT NULL,       -- ns; change detection
    mime          TEXT,
    content_hash  BLOB,                   -- skip re-extraction when unchanged
    indexed_at    INTEGER NOT NULL,
    state         INTEGER NOT NULL        -- 0=indexed 1=pending 2=tombstone 3=error
);
CREATE INDEX files_state ON files(state);
CREATE INDEX files_parent ON files(parent);

-- Extracted text, zstd-compressed (~3× on prose). Only live rows have a doc:
-- tombstoning deletes the doc, which via triggers drops the FTS postings, so the
-- query path needs no state filter. This is what keeps the on-disk body ≈ 30 %
-- of raw text while snippets still work (see below).
CREATE TABLE docs (
    id      INTEGER PRIMARY KEY,
    name    TEXT NOT NULL,
    body_z  BLOB                          -- zstd frame; NULL for empty bodies
);
-- The external-content source FTS5 reads through. wl_unzstd() is an app-defined
-- SQL function; decompression is deterministic, so 'delete' postings reproduce
-- exactly what was indexed. This is the trick that lets snippet()/highlight()
-- read original text WITHOUT a second uncompressed copy (challenge §3).
CREATE VIEW docs_v(id, name, body) AS
    SELECT id, name, wl_unzstd(body_z) FROM docs;

-- The inverted index over the view. name and body are separate columns so
-- ranking weights filename above content (Spotlight weights metadata over
-- content). The custom "wl" tokenizer = unicode61 + CamelCase/digit
-- segmentation; see §6. prefix='3' (not '2 3') matches the 3-char
-- content_min_query without paying for 2-char prefixes.
CREATE VIRTUAL TABLE files_fts USING fts5(
    name,                    -- filename tokens (high weight)
    body,                    -- extracted content tokens
    content='docs_v',
    content_rowid='id',
    tokenize = "wl remove_diacritics 2",
    prefix   = '3'
);
-- Term → document-frequency access for the query planner (§6.1 / challenge §8).
CREATE VIRTUAL TABLE files_fts_v USING fts5vocab('files_fts', 'row');
-- Sync triggers fire on docs (INSERT/DELETE/UPDATE), decompressing via
-- wl_unzstd() so FTS5 sees original text for tokenizing and for delete postings.
```

Notes:
- **No `tokenchars '_'`.** That option makes `_` *part of* a token, so
  `one_target_two` would index as one term and a search for `target` would miss
  it — the opposite of the goal (challenge §2). `unicode61` already treats
  `_`/`-` as separators; the custom **`wl` tokenizer** adds the CamelCase/digit
  boundaries unicode61 cannot see (`OneTargetTwo` → `one target two`), emitting
  segments at distinct positions so a spelled-out query is a *phrase* (§6).
- **Compressed external content.** `content='docs_v'` (a view over the
  compressed `docs` table) — FTS5 stores tokens, and snippets read text back
  through the view's `wl_unzstd()`. The alternative, storing raw body, was ~20 %
  larger overall (challenge §3/§7).
- **BM25** ranking is built in: `bm25(files_fts, 10.0, 1.0)` weights `name` 10×
  over `body`.
- FTS5's automatic **segment merging** (`automerge`, periodic `INSERT INTO
  files_fts(files_fts) VALUES('merge', …)`) is our transient→static housekeeping.
- **Snippets/highlights** come from FTS5 `snippet()` / `highlight()` — feeds the
  preview pane excerpt + match offsets (FR5). Hydrated for the top-K winners
  only, never per candidate (§6.1).

### 4.4 Indexing pipeline

```
 discover ─► classify ─► should_index? ─► extract ─► tokenize ─► write
 (crawl or   (MIME via   (roots∩!excl,    (importer  (unicode+   (upsert files
  inotify)    magic/ext)  size, hash≠)     subproc)   camel)      + files_fts)
```

1. **Discover** — initial recursive crawl of roots (bounded, throttled), then
   steady-state via `FsWatcher`. Reuses the existing `file_excludes` list.
2. **Classify** — MIME by libmagic (content sniff) with extension fallback.
3. **should_index** — inside a root, not excluded, under size cap, and
   `(mtime,size)` differs from the stored record (then confirm via `content_hash`
   to avoid re-extracting touched-but-unchanged files, FR7).
4. **Extract** — dispatch to the `Importer` for the MIME type, run in a
   **short-lived subprocess** with a wall-clock timeout, memory cap
   (`setrlimit`), and `nice`/`ionice`. Output is capped plain text. (This is
   `mdworker` + sandbox.)
5. **Tokenize + write** — upsert `files` and `files_fts` in one transaction.
   Deletes mark `state=tombstone`; a maintenance pass purges + merges.

### 4.5 Change detection (the FSEvents analog)

Linux has no single FSEvents equivalent; we abstract it as `FsWatcher`:

- **Default: `inotify`.** Recursively watch roots. Handle the realities:
  watch-descriptor limits (`fs.inotify.max_user_watches` — detect, warn,
  document raising it), directory add/remove (add/drop watches dynamically),
  renames (`MOVED_FROM`/`MOVED_TO` cookie pairing), and **queue overflow**
  (`IN_Q_OVERFLOW` → schedule a subtree reconcile). A directory deleted or moved
  *out* of the tree emits no per-file events for its contents, so it triggers a
  **subtree removal** (`Store::remove_subtree`, a single indexed range-delete
  over `path` and everything under `path/`) rather than a lone single-path drop
  that would strand the descendants until the next full reconcile (challenge
  §6.1).
- **Reconciliation scan (FR4).** On startup, after overflow, and on a **periodic
  timer** (`reconcile_interval_s`, default 15 min), walk roots and diff against
  `files` (mtime/size) to repair missed events — unchanged files are skipped
  cheaply by the mtime/size gate, so a steady-state pass is mostly `stat()`s under
  the same `nice`/`ionice`/battery throttle as the crawl. This is the robustness
  backstop Spotlight gets from the FSEvents DB, and it's the **only** freshness
  path for subtrees left unwatched when watch descriptors are exhausted (below),
  so on `ENOSPC` the daemon drops to a shorter `reconcile_interval_degraded_s`
  (default 3 min) and warns via the log + control status (`watch_degraded`).
- **Future: `fanotify`** with `FAN_REPORT_FID`/dirent events for whole-mount
  efficiency — behind the same interface. Gated because it historically needs
  `CAP_SYS_ADMIN`; not assumed in a user session.

Prior art to lean on: `tracker-miner-fs`, `recoll`, `plocate` all solve exactly
this on Linux.

### 4.6 Query path & UI integration

- New provider `ContentSearchProvider` in the launcher, running on the **existing
  async worker thread + eventfd** infra (same mechanism as the `fd` file search).
- Fires only when `enable_content` and `len(query) ≥ content_min_query`
  (default 3) — content search is deeper/slower-conceptually than name matching,
  so it starts later, exactly as Spotlight surfaces filename/app hits first.
- Query is two-phase (§6.1): phase 1 ranks rowids (`bm25`, no join/snippet),
  phase 2 hydrates the ≤ k winners:
  ```sql
  -- phase 1 (planner picks full vs bounded; full form shown)
  SELECT files_fts.rowid, bm25(files_fts, 10.0, 1.0) AS score
  FROM files_fts WHERE files_fts MATCH :q ORDER BY score LIMIT :k;
  -- phase 2, once per winning rowid
  SELECT f.path, f.name, f.mime, snippet(files_fts, 1, '⟦', '⟧', '…', 12)
  FROM files_fts JOIN files f ON f.id = files_fts.rowid
  WHERE files_fts MATCH :q AND files_fts.rowid = :rowid;
  ```
  Tombstoned rows have no doc and thus no postings, so no `state` filter is
  needed on the hot path.
- **Result fusion:** a new `ItemKind::Content` rendered as its own
  "CONTENTS" / "DOCUMENTS" section, *below* Applications and Files & Folders.
  BM25 combines with the existing recency/path-depth bonuses. The preview pane
  already exists — add the highlighted `snippet()` line.
- **Degradation:** the reader opens **strictly read-only first** (no WAL
  recovery, no `-shm` creation — SQLite ≥3.22 serves a WAL database read-only via
  a private heap wal-index), falling back to a `query_only` read-write handle
  only if that fails. `Store::availability()` classifies *why* an index is
  unusable — `NoIndex` / `VersionMismatch` / `Corrupt` / `Locked` — so the
  launcher distinguishes "no index → filename search" from "index present, zero
  content hits → show nothing" (challenge §5). Querying never blocks on the
  daemon (NFR8).

---

## 5. Technology choices (with rejected alternatives)

| Decision | Choice | Why | Rejected |
|---|---|---|---|
| Index engine | **SQLite FTS5** | Embedded, zero-admin, single file, mature, BM25, prefix/trigram tokenizers, incremental segment merge, WAL concurrent reads. Already a natural fit for a C++ app; small dep. | **Xapian** (heavier, larger dep, own storage); **Tantivy** (Rust — FFI/build friction); **hand-rolled inverted index** (re-implements posting lists, merging, crash-safety — high risk, the exact thing FTS5 already solved). |
| Change notify | **inotify** + reconcile, `FsWatcher` abstraction | Works unprivileged in a user session; ubiquitous; well-trodden. | **fanotify** (needs `CAP_SYS_ADMIN` historically) — kept as a future backend; **poll/rescan only** (misses NFR2 freshness, wasteful). |
| Extraction isolation | **Subprocess workers** w/ timeout + **cgroup v2** (`memory.max`, `pids.max`, `oom.group`) + `RLIMIT_DATA`/`RLIMIT_CPU` | Matches `mdworker`: a malformed PDF can't crash or hang the daemon (NFR6). cgroup `memory.max` bounds *charged* memory, so runtimes that reserve huge address space (GHC/pandoc) work under a real cap — unlike `RLIMIT_AS`, which they can't start under (challenge §4). | **`RLIMIT_AS`** (breaks address-space-hungry extractors); **in-process libs** (one bad parse takes down indexing). |
| Process model | **Persistent per-user daemon** | Index stays warm while launcher is closed; instant read-only queries; isolation. | **Index inside the launcher** (dies on exit, can't stay fresh, slow first query). |
| Launcher↔index | **Direct read-only SQLite (WAL)** | No IPC on the hot path; simplest, fastest. | **Query-over-socket to daemon** (adds latency + a serialization protocol for no benefit on reads). |

Extraction (all optional, degrade if absent — like missing `mdimporter`s):
- plain/code/markdown — built-in, in-process.
- HTML — built-in tag-strip (drops `<script>`/`<style>`).
- **PDF** — `pdftotext` (poppler) subprocess.
- **OOXML** (`docx`/`xlsx`/`pptx`) and **ODF** (`odt`/`ods`/`odp`) — these are ZIP
  containers, so we `unzip -p` just the text-bearing members and strip the XML
  in-process: `word/document.xml` (+headers/footers/notes) for docx,
  `xl/sharedStrings.xml` for xlsx, `ppt/slides/slide*.xml` (+notes) for pptx,
  `content.xml` for ODF. This is what makes the headline "search the contents of a
  spreadsheet" work, and it removes the heavy **pandoc** (GHC) dependency for the
  common office formats. `unzip` exits non-zero when an optional member glob
  matches nothing, so success is keyed on non-empty output. (xlsx indexes only the
  shared-string table, not the worksheets: string cells there store *indices* into
  that table plus header/footer font chrome, which would be junk in the index; a
  rare inline-string workbook falls through to pandoc.)
- **EPUB** — `unzip -p` the `.xhtml`/`.html` reading content, HTML-strip; OPF/NCX
  metadata is deliberately excluded.
- **RTF** and any OOXML/ODF variant our member map misses — fall back to **pandoc**
  (`-t plain`), or `odt2txt` for `.odt` when pandoc is absent.
- **libmagic** for MIME sniffing; extension is authoritative for the ZIP office
  formats (libmagic often reports them as `application/zip`).

---

## 6. Tokenization & match semantics

- Default: the custom **`wl` tokenizer** (`unicode61` + CamelCase/digit
  segmentation) + a `prefix='3'` index → word and word-prefix matches ("as you
  type"), Spotlight-like. `wl` wraps unicode61 and, for all-ASCII tokens, splits
  on case and letter↔digit boundaries with acronym handling
  (`XMLHttpRequest` → `xml http request`).
- **Index vs query symmetry (the correctness crux, challenge §1.3).** At index
  time each segment is emitted at its *own position*, plus the whole folded
  compound *colocated* with the first segment (so a lowercase-compound query
  still hits). At query time only the segments are emitted — no colocated
  compound — so a spelled-out term like `OneTargetTwo` compiles to the **phrase**
  `one target two`. That phrase matches `OneTargetTwo`, `one_target_two`, and
  `one-target-two` alike, but does *not* match a document that merely contains
  the lone word `one` (a colocated compound at query time would have made it an
  OR and returned that false positive — the precision bug this design avoids;
  `content_store_test` locks it in). Positions are therefore required
  (`detail=full`), which is part of why the index is position-heavy (NFR5).
- **Substring/`CONTAINS`**: word-prefix ≠ arbitrary substring. For true
  substring matching (find `target` in `mytargetfile`) FTS5 offers the
  **`trigram`** tokenizer (supports `LIKE`/substring) at the cost of a larger
  index. Decision: ship `wl`+prefix by default; expose
  `content_match = "prefix" | "substring"` to opt into a trigram index. Spotlight
  itself is closer to word/prefix on content, so `prefix` is the faithful default.

### 6.1 Query planning — bounding the worst case (challenge §8)

A global-BM25 top-K over an ultra-common single term must score that term's whole
posting list, so its cost grows with corpus size — the reason a naive p99 misses
NFR1 at 1 M docs. `Store::search` plans in two phases:

1. **Classify** the query by document frequency via `files_fts_v` (fts5vocab):
   the query is "common" only if *every* term is more frequent than
   `common_term_df` (default 20 000). One rare term makes it selective.
2. **Rank rowids** — no join, no snippet:
   - *Selective* → full `ORDER BY bm25 LIMIT k` (sub-4 ms at 1 M), guarded by a
     `query_budget_ms` deadline (SQLite progress handler); if it trips, fall back
     to (bounded).
   - *Common* → a **bounded exact-term scan**: `ORDER BY rowid DESC LIMIT
     candidate_budget` (FTS5's fast descending doclist walk — the *exact* term,
     not the `*` prefix form, which would force a full-list sort), then rank the
     budget in memory. Trades exact global ranking for a latency ceiling
     independent of corpus size; only triggers for degenerate all-common queries
     where precise ranking is moot anyway.
3. **Hydrate** metadata + `snippet()` for the ≤ k winners *only* — snippets
   decompress the body, so they must never run per candidate.

Measured worst case (`content_bench`, ultra-common term): 11 ms p99 @ 100 k,
44 ms p99 @ 1 M, vs 137 ms / 779 ms for the naive full-rank path.

---

## 7. Resource governance (the reason this exists)

- Indexing: `nice`-lowered, best-effort `ionice`, batched writes, rate-limited.
- **Adaptive throttle:** pause/slow the initial crawl on high system load or on
  battery (read `/sys/class/power_supply`), resume on idle — Spotlight's
  throttling analog.
- **Bounds everywhere:** per-file text cap (2 MB), max file size to open, skip
  binaries/media, honour excludes, hard index-size cap.
- **Extractor sandbox:** each format parser runs in a short-lived subprocess in a
  delegated cgroup v2 child (`memory.max` default 512 MB, `memory.swap.max=0`,
  `pids.max`, `oom.group=1`), placed at fork via `clone3(CLONE_INTO_CGROUP)`,
  with `RLIMIT_DATA`/`RLIMIT_CPU`/`RLIMIT_FSIZE` and `PR_SET_PDEATHSIG` as
  portable fallbacks. The systemd unit sets `Delegate=` (and drops
  `ProtectControlGroups`, which is incompatible with delegation). Without a
  delegated cgroup the rlimits alone still apply.
- **Queries are O(index)** — independent of corpus size and of how many files
  changed. This is the categorical win over live `rg`: the filesystem is read
  once at index time, never at query time.

---

## 8. Privacy & security
- Index DB under `$XDG_DATA_HOME/waylaunch/` (`0700` dir, `0600` db).
- **Privacy exclude list** (`content.exclude_paths`) — never crawled or read,
  Spotlight's "Privacy" tab analog. Ships with sensible defaults
  (`~/.ssh`, `~/.gnupg`, password stores, `~/.mozilla`, keyrings…).
- Optional `.gitignore`/dotfile awareness. No network, ever. Extraction
  subprocesses run with a minimal environment.

---

## 9. Failure modes & recovery
| Failure | Handling |
|---|---|
| Extractor crash/hang/OOM | Subprocess killed on timeout; cgroup `memory.max`+`oom.group` kills the whole extractor tree on OOM; `RLIMIT_DATA`/`RLIMIT_CPU` as portable fallback; file marked `state=error`; daemon unaffected. `PR_SET_PDEATHSIG` guarantees no orphaned extractor outlives the daemon. |
| inotify queue overflow | `IN_Q_OVERFLOW` → schedule subtree reconcile scan. |
| Directory moved-out / deleted | Subtree removal (range-delete) — descendants don't strand. |
| Watch-descriptor exhaustion (`ENOSPC`) | Detected in the watcher (fires `on_watch_limit` once); daemon warns (log + `watch_degraded`/`watch_limit_hit` in status) and shortens the periodic reconcile to `reconcile_interval_degraded_s` so unwatched subtrees still refresh. Raising `fs.inotify.max_user_watches` restores instant freshness. |
| DB corruption | Detect on open (`PRAGMA quick_check`); rebuild from scratch (index is derived data). |
| Schema/tokenizer/format change | `user_version` bump ⇒ writer rebuilds; reader reports `VersionMismatch` and degrades to filename search until the daemon has rebuilt. |
| Daemon down | Launcher opens strictly read-only; queries stale index; if none/mismatched, filename-only search. |

---

## 10. Configuration (`[content]`)

```toml
[content]
enable            = true          # master switch; also gates the UI provider
roots             = ["~"]         # defaults to search.file_roots if unset
exclude_paths     = ["~/.ssh", "~/.gnupg", "~/.mozilla"]
max_file_mb       = 8             # don't open files larger than this
max_text_mb       = 2             # cap extracted text per file
match             = "prefix"      # "prefix" (default) | "substring" (trigram)
min_query         = 3             # chars before content search fires
max_results       = 6
extractors        = ["text", "pdf", "office", "html"]  # enable/order importers
throttle_on_battery = true
reconcile_interval_s          = 900   # periodic freshness backstop (0 = off)
reconcile_interval_degraded_s = 180   # used while inotify watches are exhausted
```

The launcher reads only `enable`, `min_query`, `max_results`, `match`; the rest
are the daemon's. (Consistent with the "config reflects real, wired behaviour"
principle — nothing is parsed that isn't used.)

---

## 11. Implementation plan

Build the full daemon to the spec above — **no deliberately-minimal interim
release, no "text-only" stopgap**. The order below is *dependency* order
(store → pipeline → freshness → control → UI → hardening); each milestone lands a
*complete* subsystem, and the correctness/robustness properties (subprocess
isolation, resource caps, self-heal) are present from the start rather than
retrofitted. The end state after milestone 7 is the complete system in §4.

1. **Validation gate.** Before committing structure, confirm the core bet:
   load the §4.3 FTS5 schema, crawl a real `~`, and measure query latency
   (100k / 1M docs), index size, and crawl cost against NFR1/NFR5. This is a
   throwaway benchmark, not a product — adjust tokenizer/schema here if the
   numbers miss budget.
2. **Store + index writer.** `waylaunchd` skeleton, SQLite/WAL store, `files` +
   `files_fts` schema, transactional `IndexWriter`, `user_version` schema
   migration, `PRAGMA integrity_check` self-heal, segment-merge housekeeping.
3. **Extraction pipeline (complete).** `Importer` interface plus the *full*
   extractor set — text/code/markdown, PDF (poppler), Office/ODF, HTML — behind
   `ExtractorPool` subprocess workers with wall-clock timeout + `setrlimit` and
   libmagic MIME detection from day one. Content-hash skip (FR7), per-file text
   caps (NFR5), `state=error` isolation for bad files (NFR6).
4. **Discovery + freshness.** Throttled initial recursive crawl and `FsWatcher`
   (inotify) with change queue, rename pairing, and overflow/startup reconcile
   (FR3/FR4, NFR2).
5. **Control plane.** `waylaunchctl` + control socket: status, pause/resume,
   force reindex, runtime exclude (FR8); systemd user unit for autostart.
6. **UI integration.** `ContentSearchProvider` on the async-worker/eventfd path,
   `ItemKind::Content`, "CONTENTS" section, FTS5 `snippet()` in the preview,
   `[content]` config, read-only/degradation path (FR5/FR6, NFR8).
7. **Governance & hardening.** Adaptive throttle (load/battery), privacy-exclude
   defaults, index-size cap, plus the benchmark + robustness suites of §12 in CI
   (NFR3/6/7).

**Explicitly out of the initial build (future):** fanotify backend; richer
metadata attributes (author/kind/dates); OCR/image + semantic search (the
`mediaanalysisd` analog).

---

## 12. Testing & acceptance
- **Correctness:** golden corpus (known files/terms) → expected hits/ranks;
  extractor unit tests per format; rename/delete/overflow event tests.
- **Performance (gates NFR1/3/4/5):** synthetic corpora at 100k / 500k / 1M
  docs; assert p50/p99 query latency, daemon RSS, index-size ratio, crawl CPU.
- **Freshness (NFR2):** write a file, poll until it appears, assert ≤ 5 s.
- **Robustness (NFR6):** feed malformed PDFs / huge files / fuzzed inputs; assert
  daemon survives, queries stay live, bad files marked `error`.

---

## 13. Decisions & open questions

**Decided (2026-07-19): build our own daemon.** We will *not* wrap an existing
indexer (Tracker/`recoll`). Rationale: full control over ranking, resource
governance, and the Spotlight-faithful behaviour we're chasing; no heavy external
runtime dependency; the daemon is the core of the product, not a detail to
outsource. `waylaunchd` + SQLite FTS5 as specified in §4, built in full (no MVP /
text-only stopgap) per the §11 implementation plan.

**Resolved (2026-07-20, `content-store-hardening`):**
- *CamelCase splitting* — **custom FTS5 tokenizer** (`wl`), not a pre-tokenization
  pass. A pre-pass would rewrite text and lose the byte offsets `snippet()` needs
  (challenge §1.4); a tokenizer segments in place and keeps offsets exact. See
  §6 for the index/query symmetry that also fixes a query-precision bug.
- *Trigram-for-code* — **deferred**, not split per-type. A single index per store
  keeps ranking and the schema simple; `content_match = "prefix" | "substring"`
  remains a whole-index opt-in. Per-type mixing is future work if a need appears.

The engineering challenges surfaced while building the store — and how each was
resolved here — are catalogued in
[`CONTENT_SEARCH_STORE.md`](CONTENT_SEARCH_STORE.md) (see its **Resolutions**
section).
