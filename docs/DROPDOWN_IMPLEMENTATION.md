# Dropdown host — implementation roadmap (Option C)

> **Companion to** [`DROPDOWN_TERMINAL.md`](DROPDOWN_TERMINAL.md), which argues
> *why* waylaunch should host a dropdown rather than write a terminal emulator.
> This document is the *how*.
>
> **Target:** `waylaunch --dropdown [slot]` — a resident daemon that owns a
> terminal process and governs its visibility, placement, and lifecycle.
>
> **Written against:** Hyprland 0.56.2 (the Lua config/IPC generation).

---

## 1. Scope

**In:** session lifecycle, show/hide policy, geometry policy, focus-loss
retract, monitor following, multiple named slots, an optional layer-shell tab
strip.

**Out (load-bearing):** waylaunch never touches a pty, never parses an escape
sequence, never renders a terminal cell. The terminal is `$TERMINAL`. Phase 5's
boundary is the thing that keeps this project from becoming a terminal emulator.

### The architectural key

**Phases 1–4 need no Wayland at all.** No surface, no layer shell, no cairo, no
keyboard grab. They are: spawn a process, poll three fds, talk to a Unix socket.
That means the model is `waylaunchd` (`src/content/waylaunchd_main.cpp`) — *not*
`LauncherUI`, which is 1549 lines of surface and render management this feature
does not need until phase 5.

Consequence: the whole dropdown core is pure logic behind an interface, and
unit-tests without a compositor, exactly like `power_state_machine_test` and
`app_switcher_test` do today.

---

## 2. Spike 0 — resolved on 0.56.2

**Question: how does a Lua dispatcher target a specific window rather than the
active one?**

**Answer: pass `window = <selector-or-object>` alongside the action keys.**
Almost every `hl.dsp.window.*` dispatcher accepts an optional `window?` key;
omitted it acts on the active window. Verified live plus source
(`LuaBindingsDispatchers.cpp:hlWindowMove`, `LuaBindingsInternal.cpp:pushWindowUpval`):

```
hl.dsp.focus({ window = "class:wl-drop-0" })                    -- focus by selector
hl.dsp.window.move({ window = "class:wl-drop-0", workspace = "2", follow = false })
hl.dsp.window.move({ window = "address:0x55ce852058b0", x = 0, y = 0 })
hl.dsp.window.resize({ window = w, x = 1920, y = 480 })          -- w = hl.get_window(...)
hl.dsp.window.float({ window = ..., action = "set" })
hl.dsp.window.center({ window = ... })
hl.dsp.window.pin({ window = ..., action = "set" })
hl.dsp.window.alter_zorder({ window = ..., mode = "top" })
hl.dsp.window.set_prop({ window = ..., prop = "...", value = "..." })
```

Selector forms: `class:`, `initialclass:`, `title:`, `tag:`, `pid:`,
`stableid:`, `address:0x...`, `activewindow`, `floating`, `tiled` — or an
`HL.Window` object (converted internally to `address:0x...`). Prefer the
`address:` from `j/clients` once the slot pid is known, so two slots with
similar classes cannot collide.

Why the doc previously said otherwise: the construction-time error
(`hl.window.move: unrecognized arguments. Expected one of: direction,
x+y(+relative), workspace, into_group, out_of_group`) lists only the
exclusive *action* keys. The optional `window?` key is consumed separately
by `pushWindowUpval` in every branch, so it never appears in that message.

What was probed live (scratch `kitty --class waylaunch-spike-test`):

- Targeted `move`/`resize`/`float`/`pin`/`center`/`alter_zorder` return `ok`
  and move only the target; `activewindow` stays put.
- `move({ workspace, follow = false })` hides silently (active unchanged);
  without `follow = false` focus follows the moved window. Use silent for
  hide, following (or explicit `focus`) for show.
- A miss is safe, never falls back to the active window: `focus` miss is
  `warning: hl.focus: window not found` (`ok=false` over `repl`); `move`
  miss is a silent no-op (`ok`); `center` miss is `info: No floating window
  found`. Verified the active window's geometry was untouched after a miss.
- IPC form is `/dispatch <dispatcher-expr>`, e.g.
  `/dispatch hl.dsp.window.move({window="class:wl-drop-0", x=0, y=0})`.
  Do not wrap with `hl.dispatch(...)` over the socket (that double-wrap
  errors); `hl.dispatch(...)` wrapping is only for `hyprctl repl/eval`.

Two exceptions, both confirmed in source and live:

1. **`hl.dsp.window.bring_to_top()` takes no arguments** (`hlWindowBringToTop`
   ignores its table; `dsp_bringToTop` calls `alterZOrder("top")` with no
   window). It always raises the active window, and a passed `window=` is
   silently ignored. **Use `alter_zorder({ mode = "top", window = ... })`
   for targeted raise.**
2. **`HL.Window` objects are data-only** (properties `class/title/address/pid/
   workspace/monitor/floating/at/size/tags`, no `move`/`resize`/`focus`
   methods — `type(w.move) == nil`). So fallback 1's "object methods" half
   does not exist; but its "pass object as `window=`" half is the clean path
   when already inside Hyprland Lua. Over IPC from the daemon, use the
   `address:` selector string instead (`/eval` returns no values, so resolve
   addresses via `j/clients`).

Fallbacks 2 (focus-then-act) and 3 (dynamic `hl.window_rule`) are **not
needed**. Consequence for `HyprlandBackend::show()/hide()`: one `/dispatch`
per action with `window=<address>`, no focus games on hide.

---

## 3. Module map

```
include/waylaunch/dropdown/
    placement_backend.h     # IPlacementBackend + WindowInfo/MonitorInfo/Geometry
    hyprland_backend.h
    hyprland_events.h
    dropdown_manager.h      # the state machine — pure logic
    geometry_policy.h       # pure function
    session_supervisor.h
    dropdown_state.h        # persisted geometry override
src/dropdown/
    hyprland_backend.cpp
    hyprland_events.cpp
    hyprland_json.cpp       # minimal scanner for j/clients, j/monitors
    dropdown_manager.cpp
    geometry_policy.cpp
    session_supervisor.cpp
    dropdown_state.cpp
    dropdown_main.cpp       # the poll loop
tests/
    dropdown_manager_test.cpp
    geometry_policy_test.cpp
    hyprland_json_test.cpp
    hyprland_events_test.cpp
```

Register sources in `CMakeLists.txt:136` (`set(SOURCES ...)`) and tests in the
`-UNDEBUG` block at `CMakeLists.txt:276`.

### The seams

`IPlacementBackend` is to this feature what `IToplevelBackend`
(`include/waylaunch/switcher/toplevel_backend.h:39`) is to the switcher: the
single place compositor specifics live, and the reason the manager is testable.

```cpp
struct Geometry  { int x, y, w, h; };
struct MonitorInfo { std::string name; int x, y, w, h; double scale;
                     int reserved_top, reserved_bottom; int active_workspace; };
struct WindowInfo  { std::string address; std::string app_id; int pid;
                     Geometry geom; int workspace; bool visible; };

class IPlacementBackend {
public:
    virtual ~IPlacementBackend() = default;
    virtual std::optional<WindowInfo> find_window(const std::string& app_id) = 0;
    virtual std::optional<MonitorInfo> focused_monitor() = 0;
    virtual bool show(const WindowInfo&, const Geometry&) = 0;
    virtual bool hide(const WindowInfo&) = 0;
    virtual bool focus(const WindowInfo&) = 0;
    virtual bool supports_geometry() const = 0;   // false ⇒ degraded mode
};
```

`DropdownManager` holds an `IPlacementBackend*` and performs **no I/O**. Tests
drive it with a `FakeBackend` that records calls — the `power_manager_test`
pattern.

---

## 4. Transport: the Hyprland IPC protocol

Verified by hand against the live socket. Both live at
`$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/`.

### `.socket.sock` — request/response

Connect, write one command, read until EOF, close. No framing, no handshake.

| Command | Reply |
|---|---|
| `j/clients` | JSON array: `address`, `class`, `pid`, `at`, `size`, `workspace{id,name}`, `monitor`, `floating`, `mapped` |
| `j/monitors` | JSON array: `name`, `x`, `y`, `width`, `height`, `scale`, `focused`, `activeWorkspace{id}`, `reserved` ([left, top, right, bottom] — only the vertical insets feed placement) |
| `/dispatch <lua>` | `ok` / `error: …` / `warning: …` |
| `/eval <lua>` | `ok` — **the value is not returned over IPC** |

That last row matters: `/eval` cannot be used as a query channel, so reads go
through the `j/` JSON commands. This rules out the tempting trick of having Lua
format a compact reply for us.

### `.socket2.sock` — event stream

A long-lived connection emitting `eventname>>payload` lines. Confirmed live:

```
activewindowv2>>55ce852058b0
windowtitlev2>>55ce852058b0,◑ Dropdown terminal replacement
activewindow>>kitty,◑ Dropdown terminal replacement
```

`activewindowv2` (address only) drives focus-loss. `closewindow` drives death
detection. `focusedmon` drives monitor following.

Expose the socket fd directly so the daemon's `poll()` owns it — the pattern at
`launcher_ui.cpp:539`. Never a background thread; the repo does not do that for
event sources.

### JSON

The repo has no JSON parser (`tomlplusplus` only), and `/eval` cannot substitute.
Write a ~150-line scanner in `hyprland_json.cpp` extracting only the dozen
fields above, tested against captured fixtures. A general JSON library is a
disproportionate dependency for two known-shape payloads — and fixture tests
mean the parser is verified without a running compositor.

---

## 5. Phases

Each phase is independently shippable and closes numbered gaps from
`DROPDOWN_TERMINAL.md` §2.

### Phase 1 — the daemon and its session · closes gaps 3, 9

**Deliverables**

- `waylaunch --dropdown [slot]` in `main.cpp`, reusing `acquire_single_instance()`
  (`main.cpp:21`) with lock `waylaunch-dropdown-<slot>.lock` and `SIGUSR1` as the
  toggle signal — identical to the switcher's contract, including the
  `SIG_IGN`-until-wired guard at `main.cpp:107` that fixed the "press Tab twice"
  bug. Inherit that fix rather than rediscover it.
- `SessionSupervisor`: spawn `$TERMINAL --class waylaunch-drop-<slot>`, track the
  pid, reap `SIGCHLD`, respawn with exponential backoff (250 ms → 8 s, reset on a
  session that lives >10 s).
- **`Subprocess::spawn_tracked()`** — new. `spawn_detached()`
  (`subprocess.cpp:218`) double-forks and reparents to init, so the pid is
  unreapable by design. Respawn needs a single fork + `setsid()` returning a
  waitable pid.
- `DropdownManager` state machine: `Absent → Spawning → Hidden ⇄ Visible`, plus
  `toggle()`, `on_window_closed()`, `on_child_exited()`.
- Poll loop in `dropdown_main.cpp` over `{signalfd, toggle eventfd, event socket}`.

**Acceptance:** bind `SUPER+Tab` to `waylaunch --dropdown`. It appears on first
press. `exit` inside it, press again — a fresh terminal appears. This is the gap-3
fix and it is the one users feel first.

**Tests:** `dropdown_manager_test` — every transition, respawn backoff, toggle
while spawning, double-toggle idempotence.

---

### Phase 2 — placement policy · closes gaps 2 (partial), 5, 6, 8

**Deliverables**

- `geometry_policy.cpp` — pure: `(MonitorInfo, DropdownConfig, optional<override>)
  → Geometry`. Handles edge (top/bottom/left/right), percent width/height,
  scale, and bar `reserved` insets. Zero I/O, so it tests like `power_layout`.
- `HyprlandBackend` per Spike 0's answer: targeted `/dispatch` with
  `window=<address>` for `float`, `move` (x/y and workspace), `resize`,
  `center`, `pin`, `set_prop`, plus `alter_zorder({ mode = "top", window })`
  for z-order. Do NOT use `bring_to_top()` for targeting — it takes no
  window argument and always raises the active window. Dispatch order in
  `show()` is float → workspace → **resize → move** → raise → focus:
  Hyprland's resize preserves the window center, so moving before resizing
  drifts the position whenever the size changes (probed live).
- Hide/show by moving between a per-slot hidden special workspace
  (`special:wl-drop-<slot>`) and the **focused monitor's active workspace**, read
  from `j/monitors`. This is what makes the dropdown follow the monitor instead
  of being pinned to wherever it was born.
- Geometry recomputed on every show, so hotplugging a monitor or changing
  resolution needs no restart.

**Note on gap 2:** `alter_zorder({ mode = "top" })` raises the window above
other floats, but a fullscreen client can still cover it. Truly-above-everything is
layer-shell territory and stays out of reach in Option C. Say so in the README
rather than implying parity.

**Also here:** deleting the `fix-dropterm-spawn` workaround
(`rules.lua:60`). Because the terminal now lives on the active workspace rather
than being *toggled to* via a special workspace, children spawned from it inherit
a normal workspace and there is nothing to evict. Gap 8 closes by construction —
worth verifying explicitly, since it is the clearest proof the approach is
structurally different from the current one.

**Acceptance:** the dropdown opens on whichever monitor has focus, at the
configured fraction, below the bar; `hyprctl clients` shows children on normal
workspaces.

---

### Phase 3 — focus-loss retract · closes gap 1

**Deliverables**

- `HyprlandEventStream`: pollable fd, line parser, reconnect with backoff if
  Hyprland restarts.
- On `activewindowv2>>ADDR` where `ADDR` is not the slot's address and the slot
  is visible → hide.
- **Debounce, and be careful here.** The naive version fights itself: showing the
  window generates focus events, and a menu or picker opened *by* the terminal is
  a different address but must not trigger a retract. Needs (a) a short grace
  window after show, (b) suppression while a child of the terminal's pid holds
  focus — `j/clients` gives `pid`, so walk `/proc/<pid>/stat` for the ancestry.
- Config: `hide_on_focus_loss` (default true) — some people hate this.

**Acceptance:** click any other window → it retracts. Open a file picker from
inside it → it does *not* retract. The second half is the one that will take the
debugging.

**Tests:** `hyprland_events_test` parses a recorded event fixture;
`dropdown_manager_test` gains focus-transition cases against `FakeBackend`.

---

### Phase 4 — named slots · closes gap 10

`--dropdown notes`, `--dropdown term`, one lock file and one binding each; slot
config in `[[dropdown.slots]]`; per-slot state persistence to
`$XDG_STATE_HOME/waylaunch/dropdown.tsv` via the atomic tmp+rename in
`history.cpp:162`. Mostly parameterization if phases 1–3 kept the slot id
threaded through rather than assuming a singleton — **do that from phase 1**,
it is nearly free then and invasive later.

---

### Phase 5 — the tab strip · closes gap 4

The first phase that needs Wayland. A thin layer surface with
`keyboard_interactivity: NONE` (so it takes pointer clicks without stealing the
keyboard), rendered above the terminal, listing the slot's windows as tabs.
Switching activates the corresponding toplevel through `IToplevelBackend`
(`toplevel_backend.h:39`) — already vendored, already used by the switcher.

Reuses `Renderer` and the theme palette as-is: a tab strip is text and rects,
which is exactly what the existing renderer is good at. (The renderer's
per-call `PangoLayout` construction, `renderer.cpp:221`, is only disqualifying
for a *cell grid* — a dozen tabs is nothing.)

**Requires:** `WaylandCore` to support a partial-size anchored surface. It
currently hardcodes a fullscreen overlay — all four anchors and `set_size(0,0)`
at `wayland_core.cpp:291`. Parameterize anchor and size; the `remap_surface()`
path at `:429` re-applies the same properties and must be kept in sync (it
already duplicates this block, so the parameterization fixes a latent
divergence).

---

### Phase 6 — animation · closes gap 7, best-effort

Tune via `layerrule`/`windowrule` animations. Bounded by what the compositor
offers; a true client-owned slide needs the layer-shell terminal from Option D.
Scope this as tuning, not engineering.

---

## 6. Config schema

```toml
[dropdown]
enabled            = true
terminal           = ""        # empty → $TERMINAL, then a probe list
edge               = "top"     # top | bottom | left | right
height_percent     = 40
width_percent      = 100
hide_on_focus_loss = true
focus_grace_ms     = 150       # phase 3 debounce
respawn            = true
animation          = "slide"

[[dropdown.slots]]             # phase 4; omitting it yields one default slot
name    = "term"
command = "kitty"

[[dropdown.slots]]
name    = "notes"
command = "kitty -e nvim ~/notes.md"
height_percent = 60
```

Follow the additive convention `PowerConfig` uses (`config.h:122`): omitting the
section yields full defaults, and `enabled = false` is the documented off switch.
Parse in `config.cpp`, serialize in `Config::save()` — the repo treats
advertised-but-unparsed keys as a bug (`DESIGN.md` §1.2), so wire both directions
in the same commit.

---

## 7. Sizing

| Phase | New LOC (est.) | Notes |
|---|---|---|
| Spike 0 | — | resolved — `window=` targeting, see §2 |
| 1 | ~350 | + `spawn_tracked()` |
| 2 | ~300 | includes the JSON scanner |
| 3 | ~200 | debounce is the hard part, not the parsing |
| 4 | ~100 | free if slot-threading starts in phase 1 |
| 5 | ~350 | first Wayland phase; `WaylandCore` parameterization |
| 6 | ~0 | config tuning |
| Tests | ~400 | four suites, `-UNDEBUG` |

**~1700 LOC total, ~1250 for phases 1–4.** Phases 1–3 are the ones that remove
the daily friction and are a weekend against infrastructure that already exists.

---

## 8. Risks

- **Spike 0 resolved — with one footgun.** Targeted dispatch via
  `window=<address>` covers `show()`/`hide()` with no focus games. The
  remaining risk is `bring_to_top()` (no window arg, acts on active) being
  used where `alter_zorder({ mode = "top", window })` is meant — keep a test
  or grep guard for it.
- **Hyprland coupling.** `IPlacementBackend` isolates it. A
  foreign-toplevel-only fallback covers activate/close portably with
  `supports_geometry() == false`, so the feature degrades on other wlroots
  compositors rather than failing.
- **IPC is stable but unversioned.** Keep every command string in
  `hyprland_backend.cpp`, none inline elsewhere.
- **Phase 3 focus debounce** is the only genuinely fiddly logic. Budget real
  time for the child-process-focus case; it is what separates "retracts
  correctly" from "retracts annoyingly".
- **Scope creep toward a terminal.** Once a tab strip exists, "just add splits"
  is one step away. The no-pty boundary is the guardrail.
