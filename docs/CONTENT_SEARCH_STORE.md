# 1. Tokenization was semantically harder than the schema implied

The design wanted all of these:

```text
OneTargetTwo
one_target_two
one-target-two
```

to tokenize to the same three logical words:

```text
one
target
two
```

That sounds simple, but it hides several uncomfortable tokenizer-level problems.

## 1.1 `unicode61` does not understand CamelCase

SQLite FTS5’s `unicode61` tokenizer is based on Unicode text categories and word boundaries. It is very good at treating punctuation, whitespace, and symbol boundaries as token separators.

But it does not infer morpheme boundaries inside a continuous alphabetic run.

So this:

```text
OneTargetTwo
```

is one continuous sequence of letters.

From `unicode61`’s point of view, there is no separator between:

```text
One
Target
Two
```

because the transitions are only case transitions, not word boundaries.

So the token becomes:

```text
onetargettwo
```

not:

```text
one target two
```

That means a query for:

```text
target
```

would not match:

```text
OneTargetTwo
```

unless something else intervenes.

The challenge is that the desired behavior is not just “tokenize Unicode words”; it is “tokenize words and also split identifier-style compounds the way a human would read them.” That is a higher-level linguistic/identifier-processing expectation, not something the base tokenizer provides.

---

## 1.2 CamelCase splitting is deceptively ambiguous

Once you say “split CamelCase,” the next problem is that CamelCase is not a formal grammar.

These are easy:

```text
OneTargetTwo
oneTargetTwo
One_Target_Two
```

But real text and filenames contain awkward cases:

```text
XMLHttpRequest
iOSDevice
PDFDocument
GLRenderer
glRenderer
waylaunchd
WaylaunchD
WaylaunchDaemon
file2
file2Name
v1.2.3
2026Report
```

The desired splits are not obvious:

```text
XMLHttpRequest
```

Could reasonably become:

```text
xml http request
```

or:

```text
x m l http request
```

The first is usually what users want, but it requires acronym-boundary heuristics.

Similarly:

```text
PDFDocument
```

should probably become:

```text
pdf document
```

not:

```text
p d f document
```

But:

```text
WaylaunchD
```

is ambiguous:

```text
waylaunch d
waylaunchd
way launch d
```

The challenge is that the tokenizer has to make consistent, predictable decisions across arbitrary user filenames and document text, while preserving search recall and not producing nonsense tokens.

---

## 1.3 Tokenization must be identical at index time and query time

Whatever splitting behavior is chosen, it has to apply consistently in both directions:

- when indexing document text and filenames,
- when parsing user queries,
- when expanding prefix queries,
- when generating snippets and match offsets.

If indexing splits:

```text
OneTargetTwo
```

into:

```text
one target two
```

but the query parser treats:

```text
OneTar
```

differently, the result can be silent non-matches.

This becomes especially awkward with as-you-type prefix queries.

For example, if the user types:

```text
OneT
```

what does that mean?

Possible interpretations:

```text
onet*
one t*
one target*
onetarget*
```

Those have very different recall and precision.

The challenge is not merely “split uppercase boundaries.” It is defining a stable tokenization model that works for:

- filenames,
- prose,
- code identifiers,
- acronyms,
- mixed alphanumeric names,
- prefix queries,
- snippet highlighting.

---

## 1.4 Snippet offsets make tokenization more fragile

FTS5 `snippet()` and `highlight()` depend on token positions and offsets in the original text.

If the indexed tokens are derived from a transformed version of the text, the system must still be able to map tokens back to the original text ranges.

For example:

```text
OneTargetTwo
```

may be indexed as:

```text
one
target
two
```

but the snippet still needs to highlight the original substring:

```text
Target
```

inside:

```text
OneTargetTwo
```

That means the tokenizer cannot simply normalize the text into a new string and lose the relationship to the original byte offsets.

This becomes harder with:

- case folding,
- diacritic removal,
- Unicode normalization,
- multi-byte UTF-8 characters,
- ligatures,
- characters whose folded form has a different byte length.

The challenge is that tokenization is not just producing searchable terms; it is producing searchable terms with precise positional metadata tied to the original text.

---

# 2. The `tokenchars '_'` note was a subtle but serious spec bug

The original schema note suggested something like:

```sql
tokenize = "unicode61 remove_diacritics 2 tokenchars '_'"
```

The intention was to make underscore-separated identifiers tokenize properly.

But `tokenchars` does not mean “split on this character.”

It means the opposite:

> treat this character as part of a token.

So adding `'_'` as a token character makes underscore part of words.

That means:

```text
one_target_two
```

becomes one token:

```text
one_target_two
```

instead of three tokens:

```text
one target two
```

This directly contradicted the stated goal.

The challenge here was that the schema looked plausible. It did not produce a syntax error. It did not fail loudly. It simply changed the lexical model in the wrong direction.

This kind of bug is especially nasty because:

- the database still builds,
- queries still run,
- some searches still work,
- the failure appears as reduced recall, not an exception.

A user searching for:

```text
target
```

would fail to find:

```text
one_target_two
```

because the indexed token is not `target`; it is the whole compound.

The deeper challenge was that the design note conflated three different ideas:

1. “Make `_` searchable.”
2. “Split on `_`.”
3. “Normalize identifier-style names.”

Those are not the same thing in FTS5.

`unicode61` already treats `_` and `-` as separators by default. Adding them as `tokenchars` changes them from separators into token constituents.

So the challenge was not simply a typo; it was a mismatch between the desired lexical semantics and the FTS5 tokenizer option model.

---

# 3. FTS5 snippets require original text, which changes the storage model

The design wanted:

- an inverted index,
- BM25 ranking,
- highlighted snippets,
- match offsets,
- low disk usage.

Those goals are in tension.

FTS5 external-content tables store the index, not a second copy of the full text. That is attractive for size. But `snippet()` and `highlight()` need to read the original indexed text from somewhere.

The original schema used:

```sql
content='files'
```

but the `files` table contained metadata only:

```text
path
name
parent
size
mtime
mime
content_hash
indexed_at
state
```

It did not contain the extracted body text.

That creates a fundamental problem:

> FTS5 can match tokens, but it cannot produce a body snippet unless the original body text is available through the external-content table.

This challenge had several layers.

---

## 3.1 Snippets are not free

A snippet-capable index is not just a list of words and document IDs.

To generate a useful snippet, the system needs:

- token positions,
- token offsets,
- column text,
- original document text or a snippet-capable representation,
- enough context around matches to render an excerpt.

That is substantially more storage than a minimal inverted index.

The original NFR5 target was:

```text
index size ≤ 15–25% of indexed text volume
```

But a snippet-capable word index has to store or reference enough original text to render excerpts.

That immediately puts pressure on the disk budget.

---

## 3.2 External-content tables create consistency obligations

With an external-content FTS5 table, the FTS index and the content table must stay synchronized.

That means when a file is:

- inserted,
- updated,
- re-extracted,
- truncated,
- deleted,
- tombstoned,
- replaced after rename,

the FTS index must see the correct old and new values.

This is especially delicate for deletes and updates.

FTS5 external-content deletion is not just:

```sql
DELETE FROM files_fts WHERE rowid = ?;
```

The external-content delete command needs the original indexed column values so it can remove the correct postings.

So the store must retain enough information to issue correct FTS delete operations.

That creates a coupling between:

- metadata state,
- extracted text storage,
- FTS synchronization,
- tombstone handling,
- crash recovery.

The challenge is that the index is derived data, but derived data still has strict consistency requirements if snippets and ranking are expected to remain correct.

---

## 3.3 Snippets conflict with “read the file only once”

The design principle was:

> Read each file’s content once, at index time, never per query.

But if the original text is not stored in a snippet-capable form, then producing a snippet at query time may require reading the file again.

That creates several problems:

- the file may have changed since indexing,
- the file may have been deleted,
- the file may now be excluded,
- the file may be on slow storage,
- reading many files at query time destroys query latency,
- re-reading privacy-excluded files at query time is unacceptable.

So the snippet requirement pushes toward storing original text, while the disk requirement pushes against storing original text.

That tension was one of the central challenges of the store.

---

# 4. Extractor resource limits collided with real-world runtimes

The design called for subprocess extraction with resource limits.

That is conceptually correct: a malicious or malformed file should not be able to crash or hang the daemon.

The challenge was choosing limits that are both effective and compatible with real extractors.

The obvious limit is:

```c
RLIMIT_AS
```

which caps the process’s virtual address space.

But virtual address space is not the same thing as resident memory.

Many modern runtimes reserve enormous virtual address ranges without actually using that much physical memory.

The specific example encountered was Pandoc.

Pandoc is built on Haskell/GHC. The GHC runtime reserves a very large virtual memory region early. Under a tight `RLIMIT_AS`, Pandoc may fail to start even if its actual RSS would have been acceptable.

That creates a difficult problem.

The daemon wants to say:

> Do not let an extractor use too much memory.

But the runtime says:

> I need to reserve a large virtual address region just to initialize.

Those two statements are not compatible if memory governance is expressed purely through `RLIMIT_AS`.

---

## 4.1 Memory limits are blunt on Linux

The challenge is that Linux process resource limits are not a clean “memory cap” abstraction.

Different limits behave differently:

```text
RLIMIT_AS      virtual address space
RLIMIT_RSS     resident set size
RLIMIT_DATA    data segment
RLIMIT_CPU     CPU time
RLIMIT_FSIZE   file size
RLIMIT_NOFILE  open files
RLIMIT_NPROC   number of processes
```

Some are enforced strictly.

Some are best-effort.

Some do not cover mmap-heavy workloads well.

Some do not reflect modern allocator behavior.

So the challenge was not simply “limit extractor memory.” It was:

> How do you place a meaningful resource envelope around arbitrary extraction programs without breaking valid extractors that reserve large virtual address spaces?

This matters because the extractor set is heterogeneous:

- plain text readers,
- code parsers,
- PDF extractors,
- Office/ODF unpackers,
- HTML strippers,
- Pandoc-like runtimes,
- possibly script-based extractors.

Each has a different memory model.

A limit that safely contains a C-based text extractor may instantly break a Haskell, Java, .NET, or V8-based extractor.

---

## 4.2 The daemon must remain robust without becoming unusable

The non-functional requirements demand both:

- extraction must be isolated,
- extraction must not crash or hang the daemon,
- extractors must be resource-bounded,
- supported formats must actually work.

Those requirements can conflict.

If the limits are too strict:

- valid extractors fail to start,
- common document formats become unindexable,
- users see missing content results.

If the limits are too loose:

- a malformed PDF or Office file can consume too much memory,
- an extractor can hang,
- a pathological file can degrade the whole indexing pipeline.

The challenge was that resource governance is not a single knob. It is a policy problem spanning:

- memory,
- CPU,
- wall-clock time,
- output size,
- file descriptors,
- subprocess forking,
- runtime-specific startup behavior.

And the failure mode is subtle: the daemon may remain healthy while silently failing to index large classes of files.

---

# 5. Read-only launcher access is not simple with WAL databases

The design required:

> The launcher queries read-only and works whether or not the daemon is running.

That sounds straightforward:

```text
open index.db read-only
SELECT ... MATCH ...
```

But SQLite WAL mode complicates this.

A WAL database is not just the main database file.

It may also involve:

```text
index.db
index.db-wal
index.db-shm
```

The shared-memory file and WAL index are part of how readers see a consistent snapshot.

A purely read-only connection can fail in situations where it cannot create or use the shared-memory state.

This creates a problem when:

- the daemon is not running,
- a `-wal` file exists,
- the `-shm` file is missing,
- the launcher cannot create shared memory,
- the database was not cleanly checkpointed,
- the filesystem or permissions prevent the expected WAL sidecar behavior.

The challenge is that “read-only” is not only a SQLite open flag. It is an operational property that depends on filesystem permissions, WAL state, and recovery behavior.

---

## 5.1 Strict read-only access can reduce availability

If the launcher insists on a strictly read-only connection, it may fail to open a perfectly valid WAL database that simply needs WAL/shared-memory setup.

That violates the availability goal:

> stale-but-usable index whether or not the daemon runs.

But if the launcher opens the database read-write, it violates the spirit of “launcher queries read-only.”

Even if the launcher never issues SQL writes, opening read-write may allow SQLite to perform WAL setup or recovery-related side effects.

That creates a design tension:

- strict read-only is cleaner conceptually,
- but it can make the launcher fail exactly when the daemon is down,
- which is the moment the launcher most needs to degrade gracefully.

The challenge was defining what “read-only” really means in a WAL database:

- no SQL mutations?
- no filesystem writes at all?
- no WAL recovery?
- no shared-memory creation?
- no visible side effects?

Those are not the same thing.

---

## 5.2 Failure must be distinguishable from “no results”

Another subtlety is that the launcher must distinguish between:

```text
the index exists but has no matches
```

and:

```text
the index is unavailable
```

Those require different UI behavior.

If the index is unavailable, the launcher should degrade to filename search.

If the index is available but returns no content hits, the launcher should simply show no content results.

The challenge is that SQLite failures can appear in several forms:

- open failure,
- prepare failure,
- WAL recovery failure,
- permission failure,
- missing schema,
- corrupt index,
- locked database.

The store must classify these carefully, because treating “unavailable” as “zero results” hides failures, while treating “zero results” as “unavailable” causes unnecessary degradation.

---

# 6. Recursive inotify watching is a large state-machine problem

The design needed incremental freshness:

> Detect create/modify/delete/rename incrementally, without full rescans in steady state.

On Linux, the natural mechanism is inotify.

But recursive inotify is not a simple “subscribe to filesystem events” API. It is a stateful, lossy, watch-descriptor-based mechanism with many edge cases.

The watcher must handle:

- directories being created,
- directories being deleted,
- directories being renamed,
- files being created,
- files being modified,
- files being deleted,
- files being renamed into watched trees,
- files being renamed out of watched trees,
- renames whose two halves arrive together,
- renames whose two halves arrive separately,
- queue overflow,
- watch descriptor exhaustion,
- races between scanning and watching,
- self-moves,
- replacement of a directory by a file,
- replacement of a file by a directory,
- mount/unmount behavior,
- permission-denied subtrees.

That is a lot of state.

---

## 6.1 Rename pairing is fragile

inotify reports renames as two related events:

```text
MOVED_FROM
MOVED_TO
```

They can be paired by a cookie.

In the ideal case:

```text
MOVED_FROM /a/old.txt  cookie=42
MOVED_TO   /b/new.txt  cookie=42
```

The watcher can interpret that as a rename.

But reality is messier.

The two events may be separated across reads.

One side may be outside a watched root.

One side may be dropped due to overflow.

The cookie may not be enough if the queue is under pressure.

The source or destination directory may disappear before the event is processed.

The challenge is that if correctness depends on pairing, then every unpaired or delayed event becomes a potential consistency bug.

An unpaired `MOVED_FROM` could mean:

- the file moved out of the watched tree,
- the file was deleted but the `MOVED_TO` was lost,
- the event pair is split across reads,
- the destination is not watched.

An unpaired `MOVED_TO` could mean:

- the file moved into the watched tree,
- the source was outside the watched tree,
- the `MOVED_FROM` was lost,
- the event pair is split across reads.

The challenge is deciding what each event means when the full rename story is not available.

---

## 6.2 Overflow invalidates incremental assumptions

inotify has a queue.

If events arrive faster than they are consumed, the kernel can report:

```text
IN_Q_OVERFLOW
```

Once that happens, the watcher can no longer trust that it saw every event.

Some creates, deletes, modifies, or renames may have been lost.

That breaks the incremental model.

The system must assume that the observed event stream is incomplete and that the index may now diverge from the filesystem.

The challenge is that overflow is not a normal event. It is a meta-event that says:

> The incremental stream is no longer reliable.

At that point, the watcher must fall back to some form of reconciliation.

But reconciliation over a large home directory can be expensive.

So the system faces a tension:

- events are cheap but lossy,
- reconciliation is robust but expensive,
- freshness requirements demand quick recovery,
- resource limits forbid constant full scans.

---

## 6.3 Startup freshness is also hard

When the daemon was not running, files may have changed.

The index may be stale by:

- seconds,
- minutes,
- days,
- weeks.

On startup, the system must discover what changed without doing unnecessary work.

That requires comparing the filesystem against the stored index using metadata such as:

- path,
- mtime,
- size,
- inode,
- directory structure.

But even that is not trivial at scale.

A home directory can contain:

- huge node_modules trees,
- build caches,
- package manager caches,
- browser profiles,
- virtual machines,
- mail stores,
- media libraries,
- excluded privacy paths.

The challenge is reconciling efficiently while respecting:

- excludes,
- privacy paths,
- size caps,
- CPU limits,
- battery state,
- watch limits,
- corrupted or unreadable directories.

The watcher cannot simply “scan everything” without violating the resource envelope.

---

# 7. NFR5’s index-size target was incompatible with snippet-capable search

The original non-functional requirement said:

```text
index size target ≤ ~15–25% of indexed text volume
```

The measured reality was around:

```text
~220%
```

That is not a small miss. It is an order-of-magnitude mismatch.

The challenge was that the target assumed a much more compact representation than the implemented store actually provides.

A snippet-capable FTS5 index includes several kinds of data:

- token dictionary,
- posting lists,
- document lists,
- position information,
- column information,
- prefix index entries,
- segment metadata,
- WAL overhead,
- original text accessible through the external-content table.

Each of these costs space.

Prefix indexes are especially relevant because the design wanted as-you-type behavior.

Prefix indexes make queries like:

```text
quart
revenue
```

fast, but they do so by indexing additional prefix terms.

That improves latency but increases index size.

Snippet generation also requires access to original text or a snippet-capable representation.

That increases storage further.

---

## 7.1 The requirement mixed incompatible goals

The design simultaneously wanted:

1. full-text content search,
2. word and prefix matching,
3. BM25 ranking,
4. highlighted snippets,
5. match offsets,
6. low disk usage,
7. no per-query file reads.

Those goals are not mutually exclusive, but they cannot all be maximized at once.

Snippet support pulls toward storing or referencing original text.

Prefix indexes pull toward storing more term variants.

BM25 pulls toward maintaining term statistics and posting lists.

Low disk usage pulls toward storing less.

The challenge was that NFR5 was written as if the index could be a small compressed derivative of the text, while FR5 and the query model required a much richer structure.

---

## 7.2 Spotlight comparisons are misleading here

Spotlight may achieve compact stores through system-level integration, proprietary formats, compression, metadata stores, and OS-wide coordination.

An embedded SQLite FTS5 database is a different engineering context.

It provides:

- simplicity,
- portability,
- crash safety,
- SQL querying,
- WAL concurrency,
- easy deployment.

But it does not necessarily provide the same compression or storage density as a bespoke system-level index.

The challenge was that the size target was spiritually borrowed from Spotlight-like behavior, while the implementation substrate had different storage characteristics.

---

# 8. NFR1’s p99 latency target was algorithmically unrealistic for worst-case queries

The original latency requirement was:

```text
p50 ≤ 15 ms
p99 ≤ 50 ms at 1M indexed documents
```

The measured behavior was:

```text
p50: sub-millisecond at every scale
p99: grows with corpus size for ultra-common single-term queries
```

Example measurements:

```text
~60 ms  @ 100k docs
~155 ms @ 1M docs
```

The challenge here is not that the index is broken.

It is that BM25 ranking has worst-case behavior.

---

## 8.1 Common terms force large posting-list evaluation

An inverted index is fast because it avoids scanning the whole corpus.

For a selective query like:

```text
quarterly revenue
```

the intersection of posting lists may be small.

But for a very common single term like:

```text
the
```

or:

```text
a
```

or a domain-specific ultra-common token, the posting list may include a large fraction of the corpus.

If the query asks for ranked results, the engine may need to score many or all matching documents to determine the top K.

That means query cost can grow with the size of the posting list.

At 1M documents, an ultra-common term can have a massive posting list.

So p99 latency is no longer independent of corpus size.

---

## 8.2 p50 and p99 measure different populations

The p50 query is usually selective:

- multiple terms,
- rare terms,
- filename-biased queries,
- prefix queries with limited expansion.

Those are fast.

The p99 query is dominated by pathological cases:

- single common terms,
- broad prefix queries,
- terms with huge document frequency,
- queries that require scoring many documents.

So the system can have excellent typical latency and still miss a strict p99 target.

The challenge is that the NFR treated p99 as if it were just a slightly slower p50.

In inverted-index search, p99 is often a different algorithmic regime.

---

## 8.3 Ranking quality conflicts with early termination

One way to make common-term queries faster is to stop early.

For example:

- return the first K matches in document order,
- use a cheap heuristic ranking,
- ignore global BM25,
- prefer recent files,
- prefer shallow paths,
- use approximate scoring.

That can reduce latency.

But it changes relevance.

The challenge is that the requirement wanted both:

- strong relevance ranking,
- bounded worst-case latency.

Those are in tension when the query itself is broad.

A true top-K BM25 result may require evaluating a large candidate set.

A fast result may require sacrificing exact ranking quality.

The store, as specified, favored correct ranked search, which exposed the p99 cost.

---

# Resolutions (branch `content-store-hardening`, 2026-07-20)

Each challenge above is addressed in `src/content/store.cpp`,
`src/content/extractor.cpp`, and `src/content/fs_watcher.cpp`. Regressions are
locked in by `tests/content_store_test.cpp` and `tests/content_indexer_test.cpp`;
the performance claims are reproducible with `tests/content_bench.cpp`.

## §1 Tokenization — custom `wl` tokenizer with position symmetry

- **CamelCase (§1.1/§1.2):** a custom FTS5 tokenizer wraps `unicode61` and, for
  all-ASCII tokens, splits on case and letter↔digit boundaries with acronym
  handling (`XMLHttpRequest` → `xml http request`). Ambiguous flat-lowercase
  compounds (`onetargettwo`) carry no boundary signal and are left whole — the
  same limit Spotlight has.
- **Index/query symmetry (§1.3):** segments are indexed at *distinct positions*,
  plus the whole folded compound *colocated* at the first segment. At query time
  only segments are emitted, so `OneTargetTwo` compiles to the **phrase**
  `one target two`. Both directions agree by construction. This also fixed a
  latent **precision bug**: emitting the colocated compound at query time made a
  CamelCase query an OR of its parts, so `OneTargetTwo` matched a document
  containing only `one`. Test: `precision: camel query matches only the real
  compound`.
- **Snippet offsets (§1.4):** the tokenizer segments *in place* and passes
  original byte offsets to `xToken`, so `snippet()`/`highlight()` mark
  original-cased bytes even though the indexed terms are folded. Test:
  `snippet: preserves original casing 'Revenue'`.

## §2 `tokenchars '_'` removed

The schema never sets `tokenchars '_'` (which would make `_` part of a token and
break `target` → `one_target_two`). `unicode61` splits `_`/`-` by default; the
`wl` tokenizer adds only the case/digit boundaries. Test: cross-form equivalence
matrix (`equiv: 'target' finds the 3 boundary forms`).

## §3 / §7 Snippets + size — compressed external-content view

- Extracted text lives in `docs(body_z BLOB)`, **zstd**-compressed (~3× on
  prose). FTS5's external content points at a **view** `docs_v` that
  decompresses via an app-defined `wl_unzstd()` function, so `snippet()` reads
  original text with no second uncompressed copy (§3.1). Read-once holds: query
  time never touches the filesystem (§3.3).
- Consistency (§3.2): sync triggers on `docs` decompress deterministically, so
  `'delete'` postings reproduce exactly what was indexed. Tombstoning drops the
  doc (hence the postings) immediately, so the query path needs no `state`
  filter.
- **Honest size (§7, NFR5):** the ≤15–25 % target was never reachable for a
  snippet+position+prefix index — those features *are* the bulk. Measured
  ~203 % (100 k) … ~243 % (1 M). Compression trims the body portion (~20 % of
  total) and `prefix='3'` (down from `'2 3'`) trims the prefix index; the design
  doc's NFR5 is rewritten to state the real figure.

## §4 Extractor memory — cgroup v2, not `RLIMIT_AS`

`RLIMIT_AS` broke address-space-hungry runtimes (GHC/pandoc). Extractors now run
in a delegated cgroup v2 child (`memory.max`, `memory.swap.max=0`, `pids.max`,
`oom.group=1`), placed at fork via `clone3(CLONE_INTO_CGROUP)`, which bounds
*charged* memory rather than mapped address space. `RLIMIT_DATA` (covers brk +
committed private mappings, ignores `PROT_NONE` reservations) is the portable
fallback, alongside `RLIMIT_CPU`/`RLIMIT_FSIZE` and `PR_SET_PDEATHSIG`. The
systemd unit sets `Delegate=` and drops the incompatible
`ProtectControlGroups=`. Without delegation the rlimits alone still contain a
bad parser.

## §5 Read-only launcher access

The reader opens **strictly read-only first** (`SQLITE_OPEN_READONLY`; SQLite
≥3.22 builds a private heap wal-index, so no `-shm` write is needed), falling
back to a `query_only` read-write handle only if that fails — availability is
never sacrificed for purity (§5.1). The writer checkpoints `TRUNCATE` on close,
leaving a WAL-free file for the next reader. `Store::availability()` classifies
`Ok`/`NoIndex`/`VersionMismatch`/`Corrupt`/`Locked` so the launcher tells
"unavailable → filename search" from "available, zero hits → show nothing"
(§5.2). Tests: `avail: absent db → NoIndex`, `avail: reader on wrong version →
VersionMismatch`.

## §6 inotify state machine

- **Directory moves/deletes (§6.1):** a moved-out or deleted directory emits no
  per-file events, so the watcher issues a **subtree removal**
  (`Store::remove_subtree`, an indexed half-open range delete over `path/`).
  Tests: `dir moved out → subtree removed`, `dir deleted → subtree removed`.
- **Overflow (§6.2) / startup (§6.3):** `IN_Q_OVERFLOW` schedules a reconcile;
  startup runs a crawl + `reconcile_deletions` diff against the stored metadata,
  bounded by the same excludes/caps as the crawl. (Unchanged in this branch;
  noted here for completeness.)

## §8 Worst-case query latency — df-classified bounded planner

`Store::search` is two-phase. Phase 1 classifies via `fts5vocab` document
frequency: only if *every* term exceeds `common_term_df` is the query "common".
Selective queries take a full global-BM25 top-K (sub-4 ms @ 1 M) under a
`query_budget_ms` deadline (SQLite progress handler) that falls back to bounded
if tripped. Common queries take a **bounded exact-term** `ORDER BY rowid DESC
LIMIT candidate_budget` scan (FTS5's fast descending doclist walk — the exact
term, since the `*` prefix form forces a full-list sort), ranked in memory.
Phase 2 hydrates `snippet()` for the ≤ k winners only. Measured worst case
(ultra-common term): 44 ms p99 @ 1 M vs 779 ms for the naive path — under the
NFR1 50 ms p99 bound. Ranking quality is traded only for degenerate all-common
queries where exact ranking is meaningless anyway (§8.3).
