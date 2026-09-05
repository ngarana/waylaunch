# waylaunch ↔ zvec-grep (`zg`) — Integration Analysis

> **Status:** Analysis · **Branch:** `feat/spotlight-ui-refinement` · **Last updated:** 2026-09-04
>
> Assesses integrating [`zvec-ai/zvec-grep`](https://github.com/zvec-ai/zvec-grep)
> ("zg", npm `@zvec/zvec-grep`, Apache-2.0, Node ≥22, TypeScript + native engine)
> into waylaunch's file/content search. All numbers below were measured on this
> machine against zg 0.2.1.

---

## 1. What zg is

A local-first search layer that unifies **ripgrep (exact/regex), BM25/FTS
(lexical), and vector (semantic) search** behind one interface, powered by
Alibaba's zvec engine. Three surfaces:

| Surface | Detail |
|---|---|
| CLI (`zg`) | `zg index`, `zg query`, `zg status`, `zg server` |
| **MCP server** | Streamable-HTTP JSON-RPC at `http://127.0.0.1:7999/mcp`, loopback-only, optional Bearer token |
| Agent installs | `zg install` (Claude Code, Codex, Cursor, …) — not relevant here |

- **Index** lives in `<workspace>/.zvec-grep/`; a workspace is a *project root*
  with its own glob/type/exclude scope. It is per-root, not a whole-home index.
- **Extraction:** structure-aware for C/C++/Go/Java/JS/TS/Python/Rust, Markdown;
  plain-text chunks otherwise. Skips binaries by extension, plus size caps
  (1 MiB code, 256 MiB text by default).
- **Freshness:** server mode watches FS events (with hourly reconciliation);
  `freshness: fresh|possibly_stale` is reported per result.
- **Models:** indexed search requires an embedding model. Smallest local is
  `local/potion-code-16m-v2` (31 MiB download, static Model2Vec, fast). Remote
  (Qwen) models are strictly opt-in (`--allow-remote` / workspace grant).
- **Filename search: NOT supported.** Empirically verified: an indexed file
  whose only distinguishing token is in its *name* returns `No matches` from
  indexed search. zg indexes **content**; `zg query --rg`/`--files` still find
  names, but that is ripgrep-with-Node-startup (see §2).

## 2. Measured performance (zg 0.2.1, 3-file probe tree + real index)

| Path | Latency | Notes |
|---|---:|---|
| Node process cold start | ~200 ms | floor for *any* CLI invocation |
| `zg query --rg` (managed ripgrep, direct) | 240 ms | no index needed |
| `zg query --fts` (indexed, direct mode) | ~710 ms | Node start + open index |
| `zg query` hybrid (direct) | ~850 ms | includes query embedding |
| **MCP `tools/call`, lexical (`fts`)** | **7–15 ms** | server warm, pure HTTP |
| **MCP `tools/call`, hybrid (fts+vector)** | **~120 ms** | query embedding dominates |
| zg server daemon RSS | **383 MB** | Node + loaded embedding model |

> The MCP numbers were obtained with a bare JSON-RPC session
> (`initialize` → `Mcp-Session-Id` header → `notifications/initialized` →
> `tools/call {name: "zvec_grep_search", arguments: {root, fts, limit}}`);
> response is SSE-framed JSON with the result as compact text
> (`freshness: fresh`, `#N matchedBy=… path:lines`, `source:` preview).
> Proxy env vars must be cleared for loopback fetches (`NO_PROXY=127.0.0.1`).

## 3. Fit against waylaunch's architecture

### 3.1 What zg would *not* solve

- **Filename search (the FILES section).** zg does not index filenames. The
  Tier-1 recommendation of [`FILE_SEARCH_REVIEW.md`](FILE_SEARCH_REVIEW.md) —
  "query the index for filenames" — must land on waylaunch's own store: the
  `files` table already exists (`store.cpp:511`) and needs a name-only FTS
  index to cover files without extractable text (review §F2 caveat). zg does
  not change that work item, and its 200–240 ms CLI floor disqualifies
  `zg query --rg` as a per-keystroke path.
- **Whole-`$HOME` scope.** zg's index is per-workspace-root with `.gitignore`-
  aware scope. Indexing all of `$HOME` is not its model (and 383 MB RSS is
  6× waylaunch's own NFR4 budget for `waylaunchd`).

### 3.2 What zg uniquely buys

waylaunch's content search (FTS5 BM25) has **no semantic capability**. zg adds,
via one MCP call:

1. **`query`-semantic section ("CONTENTS · semantic")** — "where does the app
   restore preferences", "quarterly revenue" → spreadsheet hits *by meaning*,
   not token overlap. 120 ms is acceptable for a section that is *suppressed
   for queries < ~3 chars and while the user is actively typing fast*, then
   filled in.
2. **Managed ripgrep fallback** for exact text/regex when no index exists —
   but only as an explicit on-demand action, never per keystroke.

## 4. Recommended integration: read-only MCP client in the launcher

New provider `SemanticProvider` (async, `ItemKind::Content`, second CONTENTS
group), enabled only when *all* of the following hold:

1. `zg` on `$PATH` **and** `zg server status --check-ready` succeeds (health
   checked once per session, not per keystroke).
2. User opts in: `[content] zg_semantic = true` (default **off** — zg must be
   user-installed; waylaunch gains no dependency, no Node, no model download).
3. User has indexed at least one root (`<root>/.zvec-grep/` exists) and
   configured the root in config, e.g. `[content] zg_root = "~/code"`.

### 4.1 Client design (C++, no new libraries)

- HTTP/1.1 POST over loopback via plain POSIX sockets (~150 LOC; the codebase
  has no curl/http dep and MCP-over-HTTP needs none).
- One MCP session established lazily and reused; reconnect on 404 (session
  expiry) or connect failure, with exponential backoff; all failures silent →
  section simply absent (matches the NFR8 degrade philosophy).
- Parsing: extract `data: {...}` SSE lines, read `result.content[0].text`
  (compact text lines, not JSON) — parse `path:lines` + `source:` blocks with
  the same mindset as the FTS snippet path. Parse defensively; any malformed
  response → drop the section.
- **Concurrency:** the MCP call runs on the existing file-search worker thread
  *after* the file/content providers return, gated by the same generation
  counter (`file_gen_`), so keystroke churn cancels it for free (review F3).
- **Never per-keystroke blocking:** hybrid cost is ~120 ms; issue the request
  only when the previous generation's results are final (i.e. debounce to
  "typing settled", ~250 ms), and keep the last results visible until the new
  ones arrive.
- **Proxy hygiene:** a launcher inherits the session env; explicit `NO_PROXY`
  handling (or raw sockets, which bypass proxies entirely) is required —
  `fetch failed` was reproduced with proxy vars set and loopback unset.

### 4.2 Flow

```text
keystroke ──▶ file worker: FileProvider + ContentProvider   (unchanged, ≤300 ms)
          ──▶ typing settled (250 ms) ──▶ SemanticProvider:
                POST /mcp tools/call zvec_grep_search
                  {root: zg_root, query: q, limit: 3}
                → parse → append CONTENTS·semantic section
```

`waylaunchd` (C++) stays untouched: it remains the *primary* index authority.
zg is a **peer**, never a replacement — its daemon is Node-based, 383 MB RSS,
and cannot satisfy NFR3/NFR4 itself.

## 5. Rejected alternatives

| Alternative | Why rejected |
|---|---|
| Shell out to `zg query` per keystroke | 200 ms process floor + 700–850 ms measured; 5× the entire walk budget |
| Replace `waylaunchd`/FTS5 with zg index | Loses `mdfind` filters, snippet governance, the whole NFR envelope; 383 MB RSS vs ≤60 MB; per-root scope ≠ whole-home |
| Replace filename search with zg | zg does not index filenames (verified); `--rg` is 240 ms/call |
| Link zvec native engine directly | C API/ABI stability unclear at 0.2.x; drags a Node-ecosystem build into a C++ project |

## 6. Order of work (if pursued)

1. **Prereq:** land FILE_SEARCH_REVIEW Tier-1 (name-only index on the store).
2. Loopback MCP mini-client (`src/search/mcp_client.{h,cpp}`) + `zg health`
   probe; unit-test the SSE/JSON parsing against recorded fixtures.
3. `SemanticProvider` + config keys (`zg_semantic`, `zg_root`, `zg_limit`);
   session reuse + generation-gated issue point.
4. Doc: README feature bullet + this file's status flip.
