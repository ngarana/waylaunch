# waylaunch — Architecture Review & Design Roadmap

> **Scope of project (stated goal):** a faithful macOS **Spotlight** clone —
> functionally and visually — for Wayland compositors (Hyprland/wlroots), with
> **minimal resource usage**.
>
> **Scope of this document:** an honest review of the current architecture,
> maintainability, extensibility, and use of design principles (DRY / KISS /
> SOLID), followed by a target architecture and a phased roadmap.

> **Last updated:** 2026-07-24. This revision reflects the state after Phase 0
> (dead-code removal) and partial Phase 1/2 (bug fixes, layer-shell activation).

---

## 1. Executive summary

The project has a solid *low-level* foundation — a hand-rolled Wayland client, a
Cairo/Pango renderer, a `posix_spawn` subprocess wrapper, a TOML config loader,
and a small recursive-descent calculator. The dead Finder browser code has been
deleted and the worst live-path bugs (data race on render, `.desktop` rescan on
every keystroke, `system()` launch) are fixed.

However, **the headline promises are still broken or partially met:**

1. **"Minimal resources / no GTK/Qt" — now true.** The build links Cairo,
   Pango, Fontconfig, and librsvg directly. Icon lookup is a small freedesktop
   `index.theme` resolver; PNGs are loaded by Cairo and SVGs by librsvg. There
   is no GTK or gdk-pixbuf API use in the source or build files.

2. **Config is mostly decorative.** The TOML schema advertises
   `[[bindings.keys]]`, `[[modes.list]]`, and `[window]`, plus
   `[platform].use_layer_shell` — but `config.cpp` does **not parse** any of
   these keys. What *is* parsed (`[appearance]`, `[theme]`, `[search]`,
   `[[commands]]`, `[history]`, `[app_switcher]`, `[power]`, `[content]`) *is* consumed by
   the live code. The gap between schema and consumption is a maintenance trap.

3. **The codebase has grown beyond the original scope** with three new subsystems
   (app switcher, power overlay, content search) that are not documented here.

The remaining structural work is well-defined: introduce a `ResultProvider`
abstraction, extract `Layout`, make `WaylandCore` encapsulated, and drive
keys/config from TOML.

**Overall grade:** solid primitives, significant cleanup done. `B` today; the
remaining refactors are straightforward and the foundation supports an `A-`.

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
  - `LauncherUI` still owns input parsing, UTF-8 editing, theme building, layout
    math, drawing, hit-testing, and process launching. `render_frame()` is ~165
    lines interleaving layout arithmetic with draw calls and magic numbers.
    Extraction of `Layout` (pure geometry) is tracked in §7 Phase 3.
  - `WaylandCore` still exposes **almost every member public** ("for C trampolines").
    Encapsulation is effectively off. Making members private and passing through
    handler methods is tracked in §7 Phase 3.
- **O — Open/Closed:** Adding a mode still means editing multiple hardcoded
  locations. There is no `ResultProvider` abstraction. Introduction is tracked in
  §7 Phase 3.
- **L — Liskov:** fine where polymorphism exists (`Subprocess` callbacks).
- **I — Interface Segregation:** `Subprocess` is well-segregated. `WaylandCore`'s
  public surface is the opposite.
- **D — Dependency Inversion:** `LauncherUI` still `new`s its concretes
  (`WaylandCore`, `Renderer`, per-call `AppLauncher`, `Calculator`, `Clipboard`).
  No seams for testing. Injection via `ResultProvider` / `Surface` / `Launcher`
  interfaces is tracked in §7 Phase 3.

### 3.4 Extensibility & maintainability

- **Config ↔ code are badly out of sync.** The TOML file advertises
  `[[bindings.keys]]`, `[[modes.list]]`, `[window]`, and
  `[platform].use_layer_shell`. These keys are **not
  parsed by `config.cpp` at all** — they are not "ignored," they simply do not
  exist in the loader. The consumed keys (`[appearance]`, `[theme]`, `[search]`,
  `[[commands]]`, `[history]`, `[app_switcher]`, `[power]`, `[content]`) *are*
  wired to behavior.
  Either wire the remaining keys or remove them from the schema — a config file
  that advertises keys it does not parse is a maintenance trap.
- **Identity drift in docs.** `README.md`'s Architecture section omits the new
  subsystems (switcher, power, content search) and still lists `librsvg` as a
  runtime dependency and the README now matches the actual dependency list and
  architecture tree.

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

### 5.2 The one abstraction that fixes the most: `ResultProvider`

Modes become data, not `switch` statements. Adding "emoji" or "web search" later
is a new class + one registration line — nothing else changes (OCP + SRP + DIP).

```cpp
struct Query { std::string text; int max_results; };

class ResultProvider {
public:
  virtual ~ResultProvider() = default;
  virtual std::string id() const = 0;          // "applications", "files", ...
  virtual std::string group_label() const = 0; // "APPLICATIONS"
  virtual bool is_available() const = 0;       // e.g. fd/rg present
  // Called on a worker; MUST NOT touch Wayland/Cairo. Returns via the sink,
  // which marshals back to the main thread.
  virtual void query(const Query&, std::function<void(std::vector<ListItem>)> sink) = 0;
  virtual void activate(const ListItem&) = 0;  // launch / copy / open
};
```

`SpotlightController` holds `std::vector<std::unique_ptr<ResultProvider>>` built
from `[[modes.list]]`. `Tab` cycles the enabled ones; results always land on the
main thread; `render` is a pure function of `(query, items, selection, layout)`.

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

Tests now cover the new subsystems (content search, power, switcher), the
persisted history/frecency logic, and a deterministic off-screen renderer
fixture. Re-point remaining gaps at what ships:

- **Pure logic (host-buildable, no Wayland):** `Calculator` (currently
  *untested* despite being the most test-friendly unit — precedence, unary,
  functions, div-by-zero, degrees), `DesktopEntry` parser, `FuzzyMatcher`/ranking,
  config round-trip, path expansion (`~`, `$XDG_*`).
- **Providers:** table-driven tests with a fake `Subprocess` so `fd/fzf/rg`
  parsing is covered without the binaries.
- **Golden-image render tests:** `renderer_golden_test` renders to an off-screen
  Cairo surface and hashes the buffer — catching geometry/color regressions and
  the B5 color bug.
- **Live-path integration:** the launcher UI (`launcher_ui.cpp`) still needs a
  compositor-backed harness; the host-buildable history and renderer seams are
  covered without requiring a Wayland display.

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

### Phase 1 — Make the promises true (in progress)

- [x] Layer-shell protocol active; `HAS_LAYER_SHELL` defined at build time.
- [x] Single-instance guards + dismiss-on-click-outside.
- [x] Right-click → reveal file (D-Bus + xdg-open fallback).
- [x] **Drop GTK/gdk-pixbuf** dependency; minimal icon resolver + librsvg (§5.5).
- [x] Layer-shell is the *exclusive* surface path; the `xdg_toplevel` fallback
      and its runtime protocol objects were removed.

### Phase 2 — Finish live-path bug fixes (in progress)

- [x] B1/B4: worker thread no longer renders off-thread; generation counter drops
      stale results.
- [x] B2: scan `.desktop` once at startup.
- [x] B3: `posix_spawn`-based launcher with argv vector.
- [x] B7: calculator evaluated on query change, not every frame.
- [x] B5: removed the pixbuf→surface conversion; Cairo/librsvg own pixel output.
- [x] B6: collision-safe unlinked SHM files and checked allocation results.
- [x] B8: checked pipe/write setup and nonblocking stdin/output multiplexing.

### Phase 3 — Structural refactor (in progress)

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
- [~] Extract `Layout` (pure geometry): largely done already — `SpotlightLayout`
      + `relayout()` precompute `rows_`/`headers_`/`panel_total_h_`; `render_frame`
      consumes the slots. Only panel origin math remains inline.
- [ ] **Introduce `ResultProvider` (§5.2); port Apps/Files/Content/Calculator to
      it.** The one large item left — restructures the search pipeline. Deferred
      to its own focused change (see §7 note).

### Phase 4 — Wire config → behavior (2–3 days)

- [ ] Add `[[bindings.keys]]` parsing to `config.cpp`; implement `KeyMap` to drive
      `on_key` from TOML (retire hardcoded key handling).
- [ ] Add `[[modes.list]]` parsing; build provider list from config.
- [ ] Add `[window]` section parsing (position, size, anchor overrides).
- [x] Add `[history]` key parsing; wire to query-history / frecency ranking.
- [ ] Add `[platform].use_layer_shell` parsing (or remove the key if always-on).
- [ ] Delete any config keys that remain unwired.

### Phase 5 — Spotlight fidelity & polish (ongoing)

- [x] Result grouping (Top Hit + sections), preview pane, query history, frecency ranking.
- [x] Golden-image + logic test suites (§6); CI.
- [ ] Verify RSS reduction against the pre-change build.

### Quick wins (any time, < 1 hr each)

`rounded_rect`→`round_rect_path` (D2) · cache row-render block (D5) · add icon
resolver tests · update README architecture tree.

---

## 8. Open questions for the maintainer

1. **`fzf` as a runtime dependency:** piping the whole `fd` output through `fzf`
   per keystroke is heavy and non-deterministic to rank. Prefer `fd` for
   enumeration + the in-process `FuzzyMatcher` for filtering/ranking?
2. **Minimum compositor target:** a compositor implementing wlr-layer-shell is
   required; there is intentionally no regular-window fallback for GNOME/KDE.
3. **Switcher / Power / Content scope:** these subsystems are already implemented
   and shipped. Should they stay as first-class peers of the Spotlight launcher,
   or be trimmed / moved to separate binaries?
