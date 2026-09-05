# Dropdown terminal — viability review

> **Question:** the kitty + Hyprland special-workspace dropdown falls short of
> Yakuake. Is a proper, fully-fledged dropdown terminal viable as part of
> waylaunch?
>
> **Verdict:** yes — but *not* by writing a terminal emulator. Build the
> **dropdown host** (session manager + placement + tab strip) and keep an
> existing terminal as the engine. That captures ~90% of Yakuake for ~10% of the
> cost, and it is the part this repo is uniquely well-positioned to build.
>
> **Last reviewed:** 2026-09-05 against Hyprland 0.56.2.

---

## 1. What is running today

From `~/.config/hypr/`:

| Piece | Location |
|---|---|
| `kitty --class dropterm` spawned into `special:dropterm silent` at login | `modules/autostart.lua:27` |
| `SUPER + Tab` → `toggle_special("dropterm")` | `modules/binds.lua:129` |
| Windowrule: `float`, `size = monitor_w monitor_h*0.4`, `move = 0 0`, `animation = slide` | `modules/rules.lua:51` |
| Windowrule `fix-dropterm-spawn`: any non-`dropterm` window landing in the special workspace gets kicked to `+0` | `modules/rules.lua:60` |

That is the standard Hyprland recipe. It is a *workspace toggle wearing a
dropdown costume*, and the gaps below are structural to that approach — no
amount of extra windowrules closes them.

## 2. Where it falls short of Yakuake

Ordered by how much they hurt in daily use.

1. **No auto-hide on focus loss.** Yakuake retracts when you click away. A
   special workspace stays up until you press the key again. This is the single
   biggest "it doesn't feel like a dropdown" gap.
2. **Not above everything.** The terminal is an ordinary window on an ordinary
   (if special) workspace, so a fullscreen window covers it. Yakuake is an
   always-on-top overlay.
3. **Fragile lifecycle.** The kitty instance is spawned once at login. Type
   `exit` and `SUPER+Tab` silently does nothing until you respawn it by hand.
   Yakuake owns its sessions and recreates them.
4. **No owned tab strip.** kitty has tabs, but they are kitty's — its own
   keybinds, its own styling, no dropdown-level notion of named sessions. The
   Yakuake tab bar (rename, reorder, per-tab title, quick-switch) has no
   equivalent here.
5. **Placement is a static rule, not a policy.** `size = monitor_w monitor_h*0.4`
   is fixed at config time. Yakuake has a draggable bottom edge and a
   width/height slider, both persisted.
6. **Single-monitor by construction.** Hyprland special workspaces are bound to
   a monitor. The dropdown does not follow the focused monitor or the pointer.
7. **The animation is the wrong animation.** You get the *workspace* transition,
   not a window sliding down from the screen edge. `animation = slide` on a
   floating window inside a special workspace is unreliable in practice.
8. **`fix-dropterm-spawn` is a symptom, not a fix.** Programs launched *from* the
   dropdown inherit its workspace, so a rule has to evict them. Every such rule
   is a papered-over consequence of reusing a workspace as an overlay.
9. **Focus races on toggle.** Nothing guarantees the terminal has keyboard focus
   the instant it appears.
10. **One dropdown only.** Yakuake-style workflows often want a second slot (a
    scratch notes terminal, a `btop` pane) on a separate key.

## 3. Candidate architectures

### A. Terminal emulator written from scratch — rejected

Hand-rolling the VT220/xterm state machine, scrollback, reflow, mouse
reporting, and selection is a multi-year project. foot is ~30k LOC of C after
years of focused work by one developer, and it *still* draws a hard line at
features like ligatures. Not viable, and not what the ask is really about.

### B. Nested compositor hosting a real terminal — rejected

waylaunch could run a miniature compositor inside a layer surface and host
`foot` in it. Technically possible; practically absurd. It means pulling in
wlroots (precisely the heavyweight dependency `docs/DESIGN.md` §1 defines the
project against), plus nested input routing, clipboard bridging, and DPI/scale
translation. Wayland has no cross-client reparenting, so there is no cheap
version of this.

### C. Dropdown host driving an existing terminal — **recommended**

A resident `waylaunch --dropdown` daemon that owns terminal processes and
governs their placement, visibility, and lifecycle. Everything expensive is
already in this repo:

| Need | Already exists |
|---|---|
| Resident daemon, toggled by a keybinding | `main.cpp:21` `acquire_single_instance()` — flock + SIGUSR1 to the incumbent. Built and debugged for the switcher. |
| Go dormant without tearing down the Wayland connection | `WaylandCore::unmap_surface()` / `remap_surface()` (`wayland_core.h:97`) |
| Enumerate / activate / close windows | `IToplevelBackend::activate()`, `::close()` (`switcher/toplevel_backend.h:39`), over the vendored `wlr-foreign-toplevel-management` |
| Multiplexed event loop | `launcher_ui.cpp:539` — `poll()` over Wayland fd + signals + eventfds |
| Layer-shell overlay above everything | `wayland_core.cpp:286` — `LAYER_OVERLAY`, `exclusive_zone(-1)` |
| TOML config, theme palette, state persistence | `config/`, `ui/history.cpp` |

What has to be written is small and ordinary:

- **Session manager** — spawn `$TERMINAL` with a per-slot app-id
  (`waylaunch-drop-0`), reap `SIGCHLD`, respawn on death. Fixes gap 3.
- **Placement policy** — geometry as a *policy* (percent of the focused
  monitor), reapplied on show, persisted to `$XDG_STATE_HOME`. Fixes 5, 6.
- **Focus watcher** — Hyprland's event socket at
  `$XDG_RUNTIME_DIR/hypr/$HIS/.socket2.sock` (confirmed present) streams
  `activewindow`/`focusedmon`. Retract when focus leaves the slot. Fixes 1, 6.
- **Placement backend** — commands over `.socket.sock`. Keep it behind an
  interface so a non-Hyprland backend can be dropped in later, exactly as
  `IToplevelBackend` isolates the switcher from `wlr-foreign-toplevel`.
- **Multiple named slots** — `--dropdown notes`, one lock file and one binding
  each. Fixes 10.

Gaps 2 (above-fullscreen) and 7 (true slide) are compositor-controlled: they
improve to "as good as Hyprland allows" — a floating window pinned to the top
layer with a `layerrule`/`windowrule` animation — but do not become fully
client-owned. That is the honest ceiling of this option.

**Cost:** roughly 700–1000 LOC across a `src/dropdown/` module plus config and
docs. Fits the existing testing pattern — the session manager and placement
policy are pure logic and unit-test the same way
`power_state_machine_test` and `app_switcher_test` do.

### D. Native terminal on layer-shell via libvterm — viable, but out of scope

The "real" answer to gaps 4 and 7: a `waylaunch --term` layer surface, top-
anchored, `keyboard_interactivity: on_demand`, `libvterm` for the VT state
machine (0.3.3 is already installed), `forkpty` for the pty, an owned tab strip,
client-side slide by animating `set_margin` (the vendored protocol's `set_margin`
was repaired earlier, so this is available).

libvterm removes the hardest single piece — the escape-sequence parser and
screen model — but it is maybe 40% of a terminal. What remains is not small:

1. **A cell-grid renderer.** `Renderer::draw_text` (`ui/renderer.cpp:221`) builds
   and destroys a `PangoLayout` *per call*. A 200×40 grid is 8000 cells per
   frame. This path cannot be reused; a glyph atlas is required (`fcft` 3.3.3
   and `harfbuzz` 14.4.0 are installed — this is foot's stack).
2. **Damage tracking.** `submit_buffer` (`wayland_core.cpp:410`) damages the
   whole buffer on every commit. Correct for a launcher, ruinous for a terminal:
   a 1920×480 surface is 3.7 MB re-uploaded per frame. Needs row-level damage
   and previous-frame diffing.
3. **pty plumbing** — `openpty`, `TIOCSWINSZ`, `SIGCHLD`, `utempter` (installed)
   so `who`/`w` are correct.
4. **Key encoding** — the full keysym→escape table: application vs. normal
   cursor mode, `modifyOtherKeys`, Alt-as-meta, bracketed paste.
5. **Selection and clipboard.** `modes/clipboard.cpp:9` shells out to `wl-copy`.
   Acceptable for "copy this file path"; not acceptable per terminal selection.
   Needs a real `wl_data_device` + primary-selection implementation.
6. **Scrollback** — libvterm hands you `sb_pushline`/`sb_popline`; the ring
   buffer, scroll position, and search are yours.
7. **Mouse reporting** (SGR 1006) or vim/htop/tmux misbehave.
8. **Reflow on resize** — libvterm does not do it. This is genuinely hard; foot
   implemented it from scratch.

Plus 256-color/truecolor, cursor styles, OSC 8 hyperlinks, and the long tail
users notice immediately. Realistically **5–8k LOC for something usable**, and
considerably more before anyone prefers it to foot or kitty.

The deeper objection is scope. `docs/DESIGN.md` §1 states the project as a
Spotlight clone with minimal resource usage. A terminal emulator is orthogonal
to search, doubles the maintenance surface, and competes with a field of
excellent implementations. The value being asked for here is *dropdown
behavior*, not *another terminal engine*.

## 4. Recommendation

Build **C**. Explicitly decline **A** and **B**. Keep **D** on the shelf as a
possible later phase — and note that C is a prerequisite for D anyway, since
session management, placement policy, and the toggle daemon are needed either
way. Nothing built for C is wasted if D is ever revisited.

### The one idea worth stealing from D

The Yakuake tab strip (gap 4) does **not** require owning the terminal. waylaunch
can render a thin **layer surface tab strip** above the terminal window —
`keyboard_interactivity: none` so it takes pointer clicks without stealing the
keyboard — and switch tabs by activating different toplevels through the
`IToplevelBackend` the switcher already uses. Each tab is a real terminal
window; the strip is a waylaunch overlay. That yields a genuine dropdown-owned
tab bar without a single line of VT parsing, and it is only possible because
this repo already has both the layer-shell renderer and the foreign-toplevel
backend.

### Phasing

| Phase | Scope | Closes |
|---|---|---|
| 1 | `--dropdown` daemon: single slot, flock+SIGUSR1 toggle, spawn/respawn, show/hide | 3, 9 |
| 2 | Placement policy: percent-of-focused-monitor geometry, top layer, persisted | 2, 5, 6, 8 |
| 3 | Focus watcher on `.socket2.sock` → auto-retract | 1 |
| 4 | Named slots, per-slot config | 10 |
| 5 | Layer-shell tab strip over toplevels | 4 |
| 6 | Animation tuning via layerrule/windowrule | 7 |

Phases 1–3 alone remove the three complaints that make the current setup feel
wrong, and are a weekend of work against infrastructure that already exists.

## 5. Risks

- **Hyprland coupling.** Phases 2–3 use Hyprland IPC. Mitigate with a
  `IPlacementBackend` interface, mirroring `IToplevelBackend`; a
  foreign-toplevel-only fallback covers activate/close portably, just without
  precise geometry.
- **Scope creep toward D.** Once a tab strip exists, "just add splits" is one
  step from a terminal emulator. Phase 5's boundary — waylaunch never touches a
  pty — should be treated as load-bearing.
- **IPC churn.** Hyprland's socket protocol is stable but not versioned. Keep
  the command strings in one file.
