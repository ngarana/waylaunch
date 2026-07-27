# waylaunch — Architecture Review & Design Roadmap

> **Scope of project (stated goal):** a faithful macOS **Spotlight** clone —
> functionally and visually — for Wayland compositors (Hyprland/wlroots), with
> **minimal resource usage**.
>
> **Scope of this document:** an honest review of the current architecture,
> maintainability, extensibility, and use of design principles (DRY / KISS /
> SOLID), followed by a target architecture and a phased roadmap.

> **Last updated:** 2026-07-27. Phases 0–3 are complete (dead-code removal,
> promises made true, live-path bug fixes, structural refactor) and Phase 4 is
> resolved (config ↔ code are already in sync — see §7). The two open questions
> in §8 are now decided.

---

## 1. Executive summary

The project has a solid *low-level* foundation — a hand-rolled Wayland client, a
Cairo/Pango renderer, a `posix_spawn` subprocess wrapper, a TOML config loader,
and a small recursive-descent calculator. The dead Finder browser code has been
deleted and the worst live-path bugs (data race on render, `.desktop` rescan on
every keystroke, `system()` launch) are fixed.

The headline promises are now met:

1. **"Minimal resources / no GTK/Qt" — true.** The build links Cairo, Pango,
   Fontconfig, and librsvg directly. Icon lookup is a small freedesktop
   `index.theme` resolver; PNGs are loaded by Cairo and SVGs by librsvg. There
   is no GTK or gdk-pixbuf API use in the source or build files.

2. **Config ↔ code are in sync.** Every section the sample config advertises
   (`[general]`, `[appearance]`, `[theme]`, `[search]`, `[history]`,
   `[app_switcher]`, `[power]`, `[content]`, `[[commands]]`) is parsed and
   consumed; there are no advertised-but-unparsed keys. The earlier
   `[[bindings.keys]]` / `[[modes.list]]` / `[window]` / `[platform]` "trap" was
   closed by removing those stale keys — keybindings are intentionally fixed
   (Spotlight fidelity), modes come from the `[search]` enable flags, and window
   geometry from `[appearance]` (§7 Phase 4).

3. **The extra subsystems are documented here** — the app switcher (§8.2), power
   overlay, and content-search daemon are covered in §2 and the module map (§5.1).

The structural work is done: the `ResultProvider` abstraction is in place (all
five modes are provider classes), `WaylandCore`'s cross-module coupling is behind
accessors, layout is precomputed in `relayout()`, and config is fully wired. What
remains is polish (§7 Phase 5) and optional refinements.

**Overall grade:** `A-`. Solid primitives, dead code gone, live-path bugs fixed,
and the OCP/SRP seams that were missing now exist. The remaining items are
polish, not architecture.

---

## 2. Current architecture (as-is)

### 2.1 What actually runs

```
main.cpp
  └─ Config::load()                              (config/config.cpp, toml++)
  ├─ LauncherUI::init/run                        (ui/launcher_ui.cpp)   ← Spotlight launcher
  │     ├─ WaylandCore                           (core/wayland_core.cpp)  display, shm, input, exclusive layer-shell, screencopy
  │     ├─ Renderer                              (ui/renderer.cpp)        Cairo/Pango + XDG icon resolver + librsvg
  │     ├─ AppLauncher                           (modes/app_launcher.cpp) .desktop scan (cached)
  │     ├─ Calculator                            (modes/calculator.cpp)   recursive-descent
  │     ├─ Clipboard                             (modes/clipboard.cpp)    wl-copy
  │     ├─ FileWorkerLoop (worker thread + eventfd wake → main-thread render)
  │     └─ open_file_location()                  (D-Bus FileManager1 + xdg-open fallback)
  │
  ├─ AppSwitcher (--switch / --command-tab)      (switcher/*)             MRU, foreign-toplevel, signal-driven
  ├─ PowerOverlay (--power)                      (power/*)                confirm/cancel modal, destructive guard
  ├─ ContentSearchIndex                          (content/*)              SQLite FTS5 + ZSTD doc store, waylaunchd daemon
  └─ waylaunchd / waylaunchctl                   (content/*)              background indexer + control CLI
```

### 2.2 What was deleted (Phase 0 complete)

The following were removed from the source tree:

- `src/browser/*` (file_model, file_ops, tags, batch_rename, drag_drop)
- `src/ui/sidebar.cpp`, `tabs.cpp`, `toolbar.cpp`, `view_modes.cpp`,
  `info_panel.cpp`, `sort_menu.cpp`, `status_bar.cpp`, `preview.cpp`
- `src/search/finder_search.cpp`
- `src/core/keyboard_shortcuts.cpp`
- `modes/file_search.cpp`, `modes/content_search.cpp` (empty stubs)
- `render_into` / `LayoutMetrics` / `RenderItem` / `TextSegment` (dead generic renderer API)

The stale `tests/build/` artifacts from July 18 still contain object files for
some of the deleted code, but the source is gone.

### 2.3 Runtime data flow (live path — launcher mode)

```mermaid
flowchart LR
  KB[Wayland keyboard] --> WC[WaylandCore]
  WC -- key handler --> LUI[LauncherUI.on_key]
  LUI -- edits query --> US[update_search]
  US -- Apps --> AL[AppLauncher.scan+search]
  US -- Files --> FW[FileWorkerLoop (std::thread)]
  FW -- writes results_fd_ --> ML[Main poll() loop]
  ML -- reads eventfd --> AR[apply_file_results]
  AR --> RI[rebuild_items]
  AL --> RI
  RI --> RF[render_frame]
  RF --> R[Renderer/Cairo] --> BUF[SHM Buffer] --> WC
```

**Rendering only happens on the main thread.** The worker thread writes to an
`eventfd`; the main `poll()` loop reads it and calls `render_frame()`. This
fixes the original data race.

---

## 3. Design-principle assessment

### 3.1 DRY — repetition

| # | Where | Duplication | Status |
|---|-------|-------------|--------|
| D1 | `renderer.cpp` Pango boilerplate | `draw_text`, `draw_markup`, `text_width`, `text_height`, `draw_icon` monogram all repeated create-layout → font-desc → set text → measure/show → free. | **Fixed.** A `with_layout(cr, font, text, fn)` helper owns the lifecycle; all five call sites use it. Golden test confirms pixel-identical output. |
| D2 | `renderer.cpp` `rounded_rect` vs `round_rect_path` | Same arc geometry written twice. | **Fixed.** `rounded_rect` calls `round_rect_path` then `cairo_fill`. |
| D3 | `wayland_core.cpp` SHM allocation | The layer-shell path now has one allocation path in `acquire_buffer`; the old XDG configure allocation duplicate was removed. | **Fixed** |
| D4 | `app_launcher.cpp` `search()` | Lowercasing done 4× per entry on every keystroke. | **Fixed.** A lowercased `DesktopEntry::search_key` is built once at scan time; `search()` is one `find()` per entry. (No `FuzzyMatcher` existed — that was doc drift.) |
| D5 | `launcher_ui.cpp` `render_frame` | The "icon + name + description row" block was written twice (hero row and list rows). | **Fixed.** A single `draw_row(item, y, selected, icon_size)` lambda renders both; hero-ness is just a larger icon argument. |

### 3.2 KISS / YAGNI — accidental complexity

- **K1 — Empty stubs.** ~~`modes/file_search.cpp` and `modes/content_search.cpp`~~
  ~~compiled as empty translation units.~~ **Fixed in Phase 0.**
- **K2 — Two competing renderer APIs.** ~~`Renderer::render_into(...)` generic list
  renderer that nothing calls.~~ **Fixed** — deleted; only immediate-mode
  primitives remain.
- **K3 — Finder subsystem.** ~~View modes, tags, batch rename, drag-drop, tabs,
  info panel~~ — all deleted in Phase 0.
- **K4 — Debounce via thread-that-sleeps.** ~~`SearchManager::search` spawned a
  `std::thread` that `sleep_for(debounce_ms)`; `cancel()` `join()`ed it.~~ The
  current worker blocks on a `std::condition_variable` and uses a generation
  counter to drop stale results — no `sleep_for` + `join()`. The remaining
  simplification (a `timerfd` in the main `poll()`) is tracked in §7.

### 3.3 SOLID

- **S — Single Responsibility:**
  - `LauncherUI` is still the launcher's hub (input parsing, UTF-8 editing, theme
    building, drawing, hit-testing) but the biggest SRP offenders are addressed:
    result production/activation moved out to `ResultProvider`s, and layout
    geometry is precomputed in `relayout()` (`SpotlightLayout` + `rows_`/
    `headers_` slots) so `render_frame()` consumes slots instead of interleaving
    arithmetic. A dedicated `SpotlightController` (§5.1) is the remaining optional
    split.
  - `WaylandCore` **cross-module coupling is fixed** — callers use `seat()`,
    `modifier_active()`, `foreign_toplevel_manager()` accessors, not raw members.
    The `wl_listener` trampoline state stays in the header (documented C-ABI glue)
    to keep `<wayland-client.h>` out of the forward-declared public header.
- **O — Open/Closed:** **`ResultProvider` exists** (§5.2). All five modes are
  provider classes; adding one is a new class + a line in `register_providers()`.
  The activation `switch` is gone.
- **L — Liskov:** fine where polymorphism exists (`Subprocess` callbacks,
  `ResultProvider`).
- **I — Interface Segregation:** `Subprocess` and `ResultProvider` are
  well-segregated; `WaylandCore`'s public surface is now the accessor API.
- **D — Dependency Inversion:** providers are injected into `LauncherUI` as
  `ResultProvider` interfaces with their deps passed in (config, history, app
  cache, store), giving test seams. `LauncherUI` still `new`s `WaylandCore` /
  `Renderer` directly — a `Surface` seam is possible future work.

### 3.4 Extensibility & maintainability

- **Config ↔ code are in sync.** Every section `config.cpp` parses is documented
  in the sample config and consumed by the live code, and vice-versa — audited in
  Phase 4. The former `[[bindings.keys]]` / `[[modes.list]]` / `[window]` /
  `[platform]` keys that were advertised-but-unparsed have been removed from the
  schema, so the maintenance trap is closed. Adding a new mode is a
  `ResultProvider` subclass plus a `[search]` enable flag; keybindings are
  intentionally fixed for Spotlight fidelity (§8-decision context).
- **Extensibility.** New search sources drop in via `ResultProvider` (OCP). New
  overlays (like switcher/power) are new `--mode` entry points sharing
  `WaylandCore`/`Renderer`. The content index is decoupled behind the
  `waylaunchd` daemon + a read-only query path.

---

## 4. Correctness & robustness findings (live path)

Ordered by severity. Resolved items are marked; remaining open items are the
actual current risk.

| ID | Severity | File / symbol | Problem | Status |
|----|----------|---------------|---------|--------|
| B1 | **High** | `search_manager.cpp` → `launcher_ui.cpp:update_items` | Search **worker thread calls `render_frame()`**, touching Cairo + Wayland concurrently with the main-thread `wl_display_dispatch`. | **Fixed.** Worker writes to `results_fd_` (eventfd); main `poll()` loop renders on main thread. |
| B2 | **High** | `launcher_ui.cpp:refresh_applications` | Constructs a new `AppLauncher` and **re-scans every XDG dir and re-parses every `.desktop` file on every keystroke**. | **Fixed.** `scan_apps()` runs once at startup; keystrokes filter cached `apps_`. |
| B3 | **High** | `launcher_ui.cpp:launch_selected` | Launch via `system("xdg-open ...")` with unescaped paths. | **Fixed.** `launch_selected()` uses `spawn_detached()` = `fork()` → `setsid()` → `fork()` → `execvp()` with a proper `argv` vector. No `system()` in `src/`. |
| B4 | Medium | `search_manager.cpp:cancel` | `cancel()` `join()`s the in-flight worker, stalling the UI thread. | **Fixed.** Worker blocks on `std::condition_variable`; stale results dropped by generation counter. |
| B5 | Medium | `renderer.cpp:load_icon_surface` | The old pixbuf→Cairo conversion could swap red/blue and mishandle premultiplied alpha. | **Fixed.** PNGs are loaded directly by Cairo and SVGs are rendered directly by librsvg; there is no manual pixbuf conversion. |
| B6 | Medium | `wayland_core.cpp:acquire_buffer` | The old literal SHM name could collide across instances or survive a crash; allocation syscalls were unchecked. | **Fixed.** Uses an unlinked `mkstemp` file and checks `ftruncate`, `mmap`, SHM-pool, and buffer creation for failure. |
| B7 | Low | `launcher_ui.cpp:render_frame` (Calc) | `Calculator` constructed and expression re-evaluated **every frame**. | **Fixed.** Evaluated in `rebuild_app_items()` on query change only. |
| B8 | Low | `subprocess.cpp:run` | The old implementation ignored pipe/write results and could block before draining child output. | **Fixed.** Pipe/action setup is checked; stdin is written nonblocking through `poll()` while stdout/stderr are drained. |

---

## 5. Target architecture (to-be)

Keep the good primitives and add three small seams.

### 5.1 Module boundaries

```
core/         WaylandCore (private state), Buffer, event loop (poll over wl_fd + timerfd + result_fd)
gfx/          Renderer (immediate-mode Cairo/Pango primitives + with_layout helper), IconLoader (XDG theme resolver + librsvg)
input/        KeyMap: config bindings → Action enum (replaces hardcoded on_key + revives config)
app/          SpotlightController: query/selection state, orchestrates providers, owns layout
providers/    ResultProvider interface + AppProvider, FileProvider, ContentProvider,
              CalculatorProvider, CommandProvider   (each is OCP-pluggable)
platform/     Launcher (posix_spawn), Clipboard, DesktopEntry parser
config/        Config (all keys actually consumed)
switcher/     AppSwitcher (already implemented; may stay separate or fold into providers)
power/        PowerOverlay (already implemented; may stay separate or fold into providers)
content/      ContentSearchIndex, waylaunchd, waylaunchctl (already implemented)
```

### 5.2 The one abstraction that fixes the most: `ResultProvider` (AS BUILT)

Modes are data, not `switch` statements. Adding "emoji" or "web search" is a new
class + one line in `register_providers()` — nothing else changes (OCP + SRP +
DIP). The shipped interface (`include/waylaunch/providers/result_provider.h`):

```cpp
struct ProviderQuery { std::string text; std::string lower; int max_results = 6; };

class ResultProvider {
public:
  virtual ~ResultProvider() = default;
  virtual std::string id() const = 0;                 // "applications", "files", ...
  virtual bool is_available() const { return true; }  // e.g. fd present, index open
  virtual bool is_async() const { return false; }     // true → run on the worker thread
  virtual std::vector<ListItem> query(const ProviderQuery&) = 0;
  virtual bool activate(const ListItem&) = 0;          // true = handled (launch/copy/open)
};
```

Two deliberate deviations from the original sketch: `query()` **returns a vector
directly** instead of a sink — the caller owns threading, running sync providers
(calculator/commands/apps) inline and async ones (files/content) on the existing
search worker, keyed off `is_async()`; and `activate()` **returns `bool`** so
`launch_selected()` is a pure dispatch loop (first provider to claim the item
wins). `LauncherUI` currently holds the `std::vector<std::unique_ptr<ResultProvider>>`
(built from the `[search]` enable flags); factoring that into a standalone
`SpotlightController` is the one remaining optional split.

### 5.3 Single-threaded event loop (final form)

```cpp
// core/event_loop
int wl_fd  = wl_display_get_fd(display);
int tmr_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC); // debounce
int res_fd = eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC);                      // worker → main wake

poll({wl_fd, tmr_fd, res_fd}):
  wl_fd  ready → wl_display_dispatch (input, configure)
  tmr_fd fires → kick provider->query on a worker (or async subprocess)
  res_fd ready → drain completed results, render on THIS thread
```

Rendering only ever happens on the thread that owns the Wayland connection.
Debounce is a `timerfd`, not a sleeping thread. Cancellation is "drop results
whose id != current" (the id counter already exists).

### 5.4 Faithful-Spotlight requirements (visual + behavioral)

- **Layer-shell overlay is the only surface path.** CMake requires the checked-in
  wlr-layer-shell protocol and defines `HAS_LAYER_SHELL`; `WaylandCore` refuses
  to start when the compositor does not advertise the protocol. There is no
  `xdg_surface`/`xdg_toplevel` fallback. The surface uses the `OVERLAY` layer,
  `EXCLUSIVE` keyboard interactivity, and no regular-window degradation.
- **Overlay ergonomics:** dismiss on focus-loss / `Esc`, single-instance (lock
  file guard already exists for all three modes), instant show (pre-warm caches).
- **Look:** translucent panel + drop shadow + backdrop blur (screencopy) already
  sketched; add the Spotlight result grouping ("Top Hit" + category sections),
  large first-line/secondary-line rows, right-aligned metadata, and a preview
  pane later.
- **Right-click → reveal file:** implemented via freedesktop
  `FileManager1.ShowItems` D-Bus call with `xdg-open` fallback.

### 5.5 Resource-usage plan (the headline promise)

- **Icon loading is GTK-free.** The renderer parses the freedesktop
  `index.theme` metadata, walks the configured theme and hicolor fallback, and
  loads PNG through Cairo or SVG through librsvg. Failed lookups retain the
  existing monogram fallback.
- Scan `.desktop` once; `mmap`-free string parsing is fine at this scale.
- Reuse SHM buffers (already double-buffered) and only damage the panel rect.

---

## 6. Testing strategy

The suite has **14 ctest targets** covering the subsystems (content search,
power, switcher), history/frecency, the provider seam, config round-trip, and a
deterministic off-screen renderer. Remaining gaps and their intended shape:

- **Providers (host-buildable, no Wayland):** `provider_test` covers
  `CalculatorProvider` + `CommandProvider` query and activate-dispatch. The
  next additions are `AppProvider`/`FileProvider` with a **fake `Subprocess`** so
  `fd` parsing/ranking is exercised without the binary.
- **Golden-image render tests:** `renderer_golden_test` renders to an off-screen
  Cairo surface and hashes the buffer — catching geometry/color regressions and
  the B5 color bug; it verified the D1 `with_layout` refactor was pixel-identical.
- **Pure logic still worth a direct test:** the `Calculator` grammar
  (precedence, unary, functions, div-by-zero, degrees) is only exercised
  indirectly through `provider_test`; a focused `calculator_test` is cheap.
- **Live-path integration:** the launcher UI still needs a compositor-backed
  harness; the switcher's confirm→activate path in particular is only verifiable
  against a real compositor (it is compositor-focus-behaviour dependent — see the
  resident-switch note in §8.2).

---

## 7. Roadmap

Each phase is independently shippable and leaves `main` runnable.

### Phase 0 — Identity & dead-code removal ✅ COMPLETE

- [x] **Decision:** Spotlight-only; Finder moved out of the live build.
- [x] Deleted dead Finder code (`src/browser/*`, old `src/ui/*`, `finder_search`,
      `keyboard_shortcuts`).
- [x] Deleted empty stubs (`modes/file_search.cpp`, `modes/content_search.cpp`).
- [x] Removed competing renderer API (`render_into`, `LayoutMetrics`, `RenderItem`,
      `TextSegment`).
- [x] Fixed live-path bugs B1, B2, B3, B4, B7.

### Phase 1 — Make the promises true ✅ COMPLETE

- [x] Layer-shell protocol active; `HAS_LAYER_SHELL` defined at build time.
- [x] Single-instance guards + dismiss-on-click-outside.
- [x] Right-click → reveal file (D-Bus + xdg-open fallback).
- [x] **Drop GTK/gdk-pixbuf** dependency; minimal icon resolver + librsvg (§5.5).
- [x] Layer-shell is the *exclusive* surface path; the `xdg_toplevel` fallback
      and its runtime protocol objects were removed.

### Phase 2 — Finish live-path bug fixes ✅ COMPLETE

- [x] B1/B4: worker thread no longer renders off-thread; generation counter drops
      stale results.
- [x] B2: scan `.desktop` once at startup.
- [x] B3: `posix_spawn`-based launcher with argv vector.
- [x] B7: calculator evaluated on query change, not every frame.
- [x] B5: removed the pixbuf→surface conversion; Cairo/librsvg own pixel output.
- [x] B6: collision-safe unlinked SHM files and checked allocation results.
- [x] B8: checked pipe/write setup and nonblocking stdin/output multiplexing.

### Phase 3 — Structural refactor ✅ COMPLETE (bar optional follow-ups)

- [x] **D1–D5 DRY cleanups.** `with_layout` helper (D1); `rounded_rect`→
      `round_rect_path` (D2); single SHM alloc path (D3); precomputed
      `search_key` matcher (D4); single `draw_row` lambda (D5). All build-clean
      and golden-test verified.
- [x] **`WaylandCore` cross-module encapsulation.** Other modules no longer reach
      into raw members — `seat()`, `modifier_active()`,
      `foreign_toplevel_manager()` accessors added and `launcher_ui` migrated. The
      remaining "C-ABI glue" members stay in the header (documented) because the
      free-function `wl_listener` trampolines that fill them can't become class
      members without pulling `<wayland-client.h>` into the forward-declared header.
- [x] Extract `Layout` (pure geometry): `SpotlightLayout` + `relayout()`
      precompute `rows_`/`headers_`/`panel_total_h_`; `render_frame` consumes the
      slots. (Only trivial panel-origin math is inline; a standalone `Layout`
      class is not worth the indirection.)
- [x] **`ResultProvider` (§5.2) — all five modes ported.**
      - `ResultProvider` interface + `ProviderQuery`; `ListItem`/`ItemKind`
        extracted to `list_item.h`; shared path/format helpers to `search_util`;
        `spawn_detached` promoted to `Subprocess::spawn_detached`.
      - **Sync providers** (`CalculatorProvider`, `CommandProvider`,
        `AppProvider`) run in `rebuild_app_items()`; **async providers**
        (`FileProvider` via fd, `ContentProvider` via the index) run on the search
        worker, which splits their output by `ItemKind` into the file/content
        buckets the UI marshals back — the threading/marshaling is unchanged.
      - `register_providers()` builds the list from `[search]` flags + resolved
        roots/store; `launch_selected()` is now pure `provider->activate()`
        dispatch — the kind `switch` is gone. Adding a mode = a new provider class
        + one registration line.
      - [x] Provider unit tests (`tests/provider_test.cpp`) — `CalculatorProvider`
            and `CommandProvider` query + activate-dispatch, off-Wayland, proving
            the seam.
      - Follow-up (optional): lift `providers_` into a dedicated
        `SpotlightController` (§5.1); add `AppProvider`/`FileProvider` tests with a
        fake `Subprocess`.

### Phase 4 — Wire config → behavior ✅ RESOLVED

Audit finding: config ↔ code are already in sync, so this phase reduced to a
reconciliation rather than new wiring. The keys it was written to fix were stale.

- [x] **Audit:** every parsed section is documented + consumed; no
      advertised-but-unparsed keys remain (verified against `config.cpp` and the
      sample `config/waylaunch.toml`).
- [x] `[history]` parsed and wired to query-history / frecency ranking.
- [x] `[[modes.list]]` — **not needed.** Mode enable/disable is the `[search]`
      bool flags (`applications`/`files`/`calculator`/`commands`) consumed by
      `register_providers()`; category order is fixed (Spotlight fidelity).
- [x] `[window]` — **not needed.** Panel geometry is `[appearance]` (width,
      `margin_top`, sizes). A second window section would duplicate it.
- [x] `[platform].use_layer_shell` — **removed.** Layer-shell is the exclusive,
      hard-required surface path (Phase 1 / §8.1); there is no toggle.
- [x] `[[bindings.keys]]` — **intentionally omitted.** Keybindings are fixed for
      Spotlight fidelity (a KeyMap driven by TOML was considered and declined as a
      divergence, not a fix). The `on_key` handler could still be refactored to an
      internal Action table for clarity — tracked as a Phase 5 nicety, not config.

### Phase 5 — Spotlight fidelity & polish (ongoing)

- [x] Result grouping (Top Hit + sections), preview pane, query history, frecency ranking.
- [x] Golden-image + logic test suites (§6); CI.
- [ ] Verify RSS reduction against the pre-change build.

### Quick wins (any time, < 1 hr each)

- [x] `round_rect_path` is the single rounding path helper; `rounded_rect` is
      the public fill/stroke wrapper over it (D2).
- [x] Row rendering funnels through `with_layout()` — no duplicated Pango
      measure/draw block (D5).
- [x] Add icon-resolver tests · update README architecture tree.

---

## 8. Open questions for the maintainer — RESOLVED

### 8.1 Minimum compositor target → **wlroots + wlr-layer-shell, hard requirement**

**Decision:** a compositor implementing `wlr-layer-shell` is *required*; there is
intentionally **no** regular-window (`xdg-toplevel`) fallback for GNOME/KDE.

**Rationale.** The product *is* the Spotlight overlay UX: an OVERLAY-layer surface
with **exclusive keyboard**, top-anchored geometry, and dismiss-on-focus-loss. A
plain xdg-toplevel cannot deliver any of those three — it can't own the keyboard
globally, can't pin above other surfaces, and can't reliably self-dismiss. A
fallback would therefore not be a lesser version of waylaunch; it would be a
different, worse program masquerading under the same name. The `xdg-toplevel`
path was already deleted in Phase 0/1, so this decision simply ratifies the
as-built state. Practically this covers Hyprland, sway, river, Wayfire, and the
other wlroots-family compositors — the intended audience.

### 8.2 Switcher / Power / Content scope → **keep, as one binary + one daemon**

**Decision:** the launcher, `--switcher`, and `--power` overlays stay as
first-class `--mode` faces of the single `waylaunch` binary. Content search stays
correctly separate as the `waylaunchd` / `waylaunchctl` daemon pair. Nothing is
trimmed or folded into `ResultProvider`.

**Rationale.**

- **Why one binary for launcher/switcher/power.** All three are full-screen modal
  layer-shell overlays that share the *same* substrate: `WaylandCore` (surface,
  layer-shell handshake, keyboard grab, blur backdrop), `Renderer` (Cairo/Pango +
  `round_rect_path`/`with_layout`), `Config`, and the single-instance flock. They
  differ only in their input state machine and what they paint. Splitting them
  into separate executables would duplicate the ~17 MB shared-lib footprint per
  process and fork the surface-lifecycle code that was the hardest to get right
  (map/unmap/remap handshake, cold-start grab races). One binary, selected by
  `--mode`, is the KISS answer and matches how they're actually invoked (distinct
  compositor keybinds).

- **Why NOT fold switcher/power into `ResultProvider`.** `ResultProvider` produces
  *list items* for the unified query pipeline (a search box that ranks/merges
  rows). Switcher and power are a different category: modal overlays with their
  own dedicated input grammar (hold-modifier cycling; confirm-countdown dialogs)
  and their own renderers. Forcing them through the item/query seam would bend the
  abstraction to fit two things it wasn't meant to model. They correctly live in
  `src/switcher/` and `src/power/` beside the launcher, not inside it.

- **Why content stays a separate daemon.** Indexing is a long-lived, resource-
  governed background job (inotify watches, crawl throttling, an FTS5 store with a
  size cap) with a lifecycle wholly unlike a spawn-on-keypress overlay. The
  launcher touches it only as a **read-only** query client. Process separation
  here is the right boundary, and is already in place.
