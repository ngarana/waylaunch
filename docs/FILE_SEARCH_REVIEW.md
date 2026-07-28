# waylaunch — File Search: Review of the `fd` → Native Walk Refactor

**Status:** Review · **Branch:** `feat/spotlight-ui-refinement` · **Last updated:** 2026-07-27

Review of the uncommitted change that replaces the `fd` subprocess in
`FileProvider` with an in-process filesystem walk, assessed against waylaunch's
stated goal of **instant results with minimal resource usage**.

The refactor is functionally faithful and `tests/file_provider_test.cpp` is a
genuine addition — it passes and covers exclude pruning, glob excludes,
case-insensitivity, folder-over-file ranking, `max_results` truncation, missing
roots and empty roots. But measured against the goal it regresses on both axes.

> **The framing that matters.** `fd` was doing more work than the replacement
> does: hidden-directory pruning, `.gitignore` pruning, multithreaded traversal,
> and no symlink following. Dropping the dependency removed those optimizations,
> it did not merely remove a binary. `docs/CONTENT_SEARCH.md` already rejects
> O(corpus)-per-keystroke search for content ("unbounded in latency, and
> violates waylaunch's minimal-resources goal"). This change introduces exactly
> that shape for filenames.

---

## 1. Measurement methodology

All numbers below are reproducible on the review machine and were produced with
the **shipped code** and the **shipped default excludes** from
`src/ui/launcher_ui.cpp:415`:

```
{".git", "node_modules", ".cache", "target", ".venv",
 "__pycache__", ".cargo", ".rustup", "go/pkg", ".local/share/Trash"}
```

- **Corpus:** `$HOME` = 144 GB on local disk, partially warm dentry cache.
- **Harness A:** links the real `FileProvider` and issues one `query()` per
  keystroke of a word, exactly as `kick_file_search()` does.
- **Harness B:** replicates the walk in `FileProvider::query()` line-for-line
  and additionally reports termination cause, directories opened, entries
  scanned, and residual queue depth.

Relevant hidden-directory sizes on the corpus, by entry count:

| Directory | Entries | Excluded by default? |
|---|---:|---|
| `~/.local` | 524,145 | **No** (see §7) |
| `~/.cache` | 277,775 | Yes |
| `~/.config` | 83,601 | **No** |
| `~/.npm` | 78,700 | **No** |
| `~/.mozilla` | 47,295 | **No** |
| `~/.cargo` | 32,017 | Yes |
| `~/go` | 27,803 | **No** (see §7) |

---

## 2. Findings, ranked by impact on the stated goal

### F1 — The walk never completes; results are an artifact of the timeout

`src/providers/file_provider.cpp:60-63` bounds the walk at `kMaxVisited =
200000` and `kDeadlineMs = 300`. On this corpus **every query terminates on the
deadline**, never on exhaustion:

```
query re          324.0 ms    6 hits
query rep         310.2 ms    6 hits
query repo        309.4 ms    6 hits
query repor       331.7 ms    6 hits
query report      327.4 ms    6 hits
---
total walk time for typing "report": 1617.1 ms

terminated by: deadline      309.0 ms
  dirs opened      : 20480
  entries scanned  : 200517
  name matches     : 346
  dirs left queued : 20145      ← ~half the tree never visited
  peak queue depth : 20479
```

The BFS abandons with **20,145 directories still queued**. What comes back is
therefore a function of BFS ordering and how warm the page cache happened to be
— not of what is on disk. Anything below the abandoned frontier is
*permanently* unreachable, and the result set is non-deterministic between runs.

`fd` had the same O(filesystem) shape, but pruned hard enough (hidden +
`.gitignore`) and parallelized enough to actually finish.

Typing a six-character word costs **1.6 s of continuous full-tilt directory
scanning**. Query coalescing in `file_worker_loop()` caps user-visible *latency*
at roughly 2 × deadline, but does nothing about the CPU and dentry-cache burn.

### F2 — An index containing every filename is already open in the same process

`src/content/store.cpp:511` defines the `files` table:

```sql
CREATE TABLE IF NOT EXISTS files(
  id INTEGER PRIMARY KEY,
  path TEXT UNIQUE NOT NULL,
  name TEXT NOT NULL,          -- ← filename, already stored
  parent TEXT NOT NULL,
  size INTEGER NOT NULL,
  mtime INTEGER NOT NULL,
  ...);
```

It is kept fresh incrementally by `waylaunchd` via inotify plus a periodic
reconcile backstop. `src/content/store.cpp:542` additionally indexes the **`name`
column** in FTS5, with a trigram tokenizer in `Substring` mode (true substring
search) or `prefix='3'` in `Prefix` mode.

`include/waylaunch/content/store.h:40` states the intended direction outright:

> `NoIndex`/`VersionMismatch` → **degrade to** filename search

The walk is supposed to be the *degraded* path. `README.md` already claims
content queries are "O(index-lookup), not O(filesystem)"; after this refactor
filename search is the one search path in waylaunch that is not. **This is the
central suboptimality: a second, slower path was built alongside the fast one.**

Caveat to design around: `files_fts` uses `content='docs_v'`, so only files with
extracted text have an FTS row. Files without extractable content (images,
binaries, archives) still live in `files` and would need either an index on
`name` or a separate names-only FTS table.

### F3 — No debounce, no cancellation

`kick_file_search()` (`src/ui/launcher_ui.cpp:825`) fires on every keystroke.
`FileProvider::query()` accepts no cancellation token, so an in-flight 300 ms
walk cannot be abandoned when the query changes — the generation check at
`src/ui/launcher_ui.cpp:889` runs only *after* `query()` returns.

Worst-case post-keystroke latency is therefore ≈ 2 × deadline, and a typing
burst keeps a core pinned for its whole duration.

---

## 3. Correctness and robustness regressions vs `fd`

### F4 — Hidden directories are now walked (the main cost driver)

The removed argv carried no `--hidden` and no `--no-ignore`, so `fd` skipped
every dotdir and honoured `.gitignore`. The walk has no equivalent policy, so
the directories in the §1 table marked "No" are all in scope — over 700,000
entries of pure noise on this corpus.

This is both a cost regression and a **result-quality** regression: dotfile
noise now competes with real documents for the six result slots.

### F5 — Symlinks are followed → infinite traversal

`entry.is_directory(ec)` at `src/providers/file_provider.cpp:93` follows
symlinks. Reproduced with a three-directory tree containing a single `../..`
symlink — all six result slots filled with the same file:

```
loop/a/b/target_file.txt
loop/a/b/up/a/b/target_file.txt
loop/a/b/up/a/b/up/a/b/target_file.txt
loop/a/b/up/a/b/up/a/b/up/a/b/target_file.txt
loop/a/b/up/a/b/up/a/b/up/a/b/up/a/b/target_file.txt
loop/a/b/up/a/b/up/a/b/up/a/b/up/a/b/up/a/b/target_file.txt
```

`fd` does not follow symlinks by default. Beyond loops, this lets a walk rooted
at `~` escape into `/`, and — with no `st_dev` check — descend into network and
FUSE mounts where a single `readdir` can block for seconds.

**Fix:** test `entry.is_symlink()` / use `symlink_status`, and compare `st_dev`
against the root's to stay on one filesystem.

### F6 — `++it` is the throwing overload → process termination

`src/providers/file_provider.cpp:86` uses `for (; it != end; ++it)`.
`std::filesystem::directory_iterator::operator++()` throws `filesystem_error` on
failure. There is no `try`/`catch` in `query()`, in `file_worker_loop()`, or
anywhere in `src/ui/launcher_ui.cpp` / `src/main.cpp`.

A directory vanishing mid-walk — near-certain when traversing cache-adjacent
churn — throws on the worker thread and terminates the launcher. The old
subprocess could only fail and return nothing.

**Fix:** `it.increment(ec)`.

### F7 — Uninitialized read

`src/providers/file_provider.cpp:95-97`:

```cpp
struct stat st;                                       // not value-initialized
is_dir = (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
if (!is_dir && !S_ISREG(st.st_mode)) continue;        // UB when stat() failed
```

When `stat()` fails, `S_ISREG(st.st_mode)` reads uninitialized memory. The
sibling declaration at `:106` (`struct stat st{}`) is value-initialized; this one
is not.

### F8 — Two default excludes can never match

`name_excluded()` matches against a **basename**, but `go/pkg` and
`.local/share/Trash` are path fragments. Both are dead entries in
`src/ui/launcher_ui.cpp:415` and `config/waylaunch.toml` — a silent semantic
change from `fd`'s path globs, and the reason `~/go` and the Trash appear as
"not excluded" in the §1 table.

**Fix:** match patterns containing `/` against the root-relative path, or amend
the defaults to basenames.

---

## 4. Efficiency findings

### F9 — `matches_exclude` allocates a DP table per entry, per pattern

`src/providers/file_provider.cpp:30` builds a `vector<vector<bool>>` of size
(n+1)×(m+1) for **every entry × every glob pattern**. The default excludes
contain no globs, so this is dormant out of the box — but `config/waylaunch.toml`
and `doc/waylaunch.1` both advertise globs (`e.g. *.tmp`), and
`tests/file_provider_test.cpp:68` exercises them.

Adding three ordinary globs (`*.tmp`, `*.o`, `*.pyc`) to the defaults:

| Excludes | Entries scanned / 300 ms | Dirs opened |
|---|---:|---:|
| Default (no globs) | 200,517 | 20,480 |
| + `*.tmp`, `*.o`, `*.pyc` | 90,975 | 2,048 |

**2.2× throughput loss and 10× coverage loss** for three patterns a user would
reasonably add.

**Fix:** precompile excludes once in the constructor — `unordered_set` for
literals, an allocation-free two-pointer `*`/`?` matcher (~15 lines) for globs.
Note also that `[` is treated as a glob marker at `:25` but character classes are
not implemented by the DP.

### F10 — Unbounded `hits`, then a full sort for six results

`src/providers/file_provider.cpp:133` sorts every match to keep
`max_results_` (default 6). Measured: 346 matches for `"report"` in *half* the
tree; a two-character query yields thousands. Use a bounded min-heap of size
`max_results_`.

### F11 — Per-entry allocations

`entry.path().string()` at `:92` allocates a full path for every entry, though
it is needed only for directories and for matches; `to_lower(name)` at `:100`
allocates again. Roughly 400,000 allocations per keystroke on this corpus.

### F12 — BFS queue holds a whole tree level

Peak queue depth 20,479 path strings (~2 MB of transient churn). DFS with an
explicit stack, or `openat`-relative names, bounds this to the tree depth.

### F13 — Double `stat()` on every match

`entry.is_directory(ec)` at `:93` then `stat()` again at `:107`.
`directory_entry` caches its status, and the second call is only needed for
`st_mtime` — `entry.last_write_time(ec)` covers it.

### F14 — Deadline granularity

`src/providers/file_provider.cpp:79` checks the clock only in the **outer** loop
and only every 1024 directories. Measured overshoot: 309–338 ms against a 300 ms
budget. A single huge directory (Maildir, `~/Downloads`) receives **zero** checks
for its entire duration.

### F15 — Dead override

`FileProvider::is_available()` at `:52` now returns an unconditional `true`.
Drop the override and let `ResultProvider`'s default serve it.

---

## 5. Test coverage gaps

`tests/file_provider_test.cpp` passes and covers the happy paths well. Gaps that
map onto the findings above:

| Gap | Finding |
|---|---|
| Symlink loop in the walked tree | F5 |
| Directory that cannot be opened / disappears mid-iteration | F6 |
| A `go/pkg`-style path pattern in `excludes` | F8 |
| Deadline / `kMaxVisited` termination behaviour | F1, F14 |
| Hidden-directory policy | F4 |

---

## 6. Recommended order of work

**Tier 1 — restores "instant"**

1. **Query the index for filenames.** Match against `files.name` /
   `files_fts.name` when `content_store_` is open; keep the walk strictly as the
   no-index fallback, which is what `store.h:40` already assumes. Requires
   deciding how to cover files with no extracted doc (§F2 caveat).
2. **Interim step with no schema work:** walk **once** per session (or prewarm on
   startup) into an in-memory path vector, then filter that vector on each
   keystroke. Substring-filtering 300k cached strings costs ~1–2 ms versus the
   current ~310 ms, and keeps the zero-dependency goal.

**Tier 2 — make the fallback walk correct and cheap**

3. Skip hidden directories by default (config toggle for opt-in); do not follow
   symlinks; stay on one device. — F4, F5
4. `it.increment(ec)`; value-initialize the `struct stat`. — F6, F7
5. Precompile excludes; bounded top-K; move the deadline check into the inner
   loop. — F9, F10, F14
6. Fix or remove the path-fragment excludes. — F8

**Tier 3 — stop generating work that is already stale**

7. Debounce `kick_file_search()` by ~60–80 ms.
8. Pass a `std::atomic<uint64_t>*` generation through `ProviderQuery` and check
   it in the walk's inner loop so a superseded walk aborts immediately. — F3

Tiers 2 and 3 are contained. **Tier 1 is the only work that actually delivers
the stated goal**, and it determines the shape of everything else — worth
settling before the smaller fixes lock in the walk's current structure.
