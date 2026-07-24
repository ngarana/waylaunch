# Power Manager — User Requirements Document

> **Status:** IMPLEMENTED on `feature/power-manager` (forked from `main` after
> the `feature/command-tab-switcher` merge).
> **Stated goal:** a standalone, keyboard-first, Wayland-native power-actions
> overlay for `waylaunch` — Lock · Restart · Exit · Hibernate · Suspend ·
> Shutdown — visually and behaviourally modeled on the macOS power dialog.
> **Amendment (2026-07-23):** the overlay is NOT full-screen. It renders as a
> compact switcher-style HUD — the same centered frosted-glass card, geometry,
> and selection pill as the Alt+Tab switcher — with one row of action cards.
> §2.1/§4.5 as written below are superseded accordingly (single row instead of
> a 2×3 full-screen grid; ↑/↓ map to prev/next). Each card is **self-labelled**,
> so unlike the switcher there is no title pill naming the selection beneath the
> HUD; the selected card's own label goes bold/full-strength instead.
> **Amendment 2 (2026-07-23):** refinements over §4.4/§4.5 as drafted:
> icons are hand-drawn vector glyphs (`src/power/power_glyphs.cpp`) in round
> buttons — no icon-theme lookups; the confirmation dialog is glassmorphic
> (backdrop + tint + rim, radius 28, fully-round pill buttons) and carries an
> auto-confirm **countdown** (`[power].countdown_seconds`, default 60, 0 = off)
> shown as a depleting ring around the action glyph plus a counter in the
> confirm button; ←/→/Tab move button focus inside the dialog and Return/Space
> press the focused button (Esc still cancels).
> **Amendment 3 (2026-07-23):** §4.1's default commands were wrong for systemd
> hosts — systemd's `loginctl` has no reboot/suspend/poweroff/hibernate verbs
> (only elogind's does); those are `systemctl` verbs. Defaults now use
> `systemctl`, and `PowerActionBackend::resolve_argv` normalizes the four power
> verbs at action time to whichever binary exists (systemctl on systemd,
> loginctl on elogind) — same action-time-choice principle as the Exit
> fallback. Failed commands are reported on stderr (failure reporting, not the
> §7-prohibited command logging).
>
> **Cross-cutting principles:** DRY · KISS · SOLID · no god modules ·
> low cyclomatic complexity · minimum resource usage · minimum dependencies.

---

## 1. Background and context

The `waylaunch` project now hosts three distinct keyboard-grabbing overlays that
share the same low-level Wayland/Cairo/Pango foundation but are **decoupled** at
the feature level:

| Mode | Binary trigger | Single-instance lock | Lifespan |
|------|----------------|----------------------|----------|
| App launcher      | `waylaunch`          | `waylaunch-launcher.lock`   | toggled (Super+D) |
| App switcher      | `waylaunch --switch` | `waylaunch-switcher.lock`  | resident (Alt+Tab) |
| **Power manager** | `waylaunch --power` (proposed) | `waylaunch-power.lock` | one-shot (dismiss on confirm/cancel) |

Power manager is the third such overlay. It must reuse the existing
`waylaunch::Renderer`, `waylaunch::Config`, `waylaunch::WaylandCore`
(`src/core/wayland_core.cpp`), and the single-instance guard pattern in
`src/main.cpp::acquire_single_instance` — **without polluting them with
power-specific knowledge.** Each overlay's feature logic lives in its own
directory under `src/<feature>/` and `include/waylaunch/<feature>/`, mirroring
the `src/switcher/` layout.

---

## 2. Goals

1. Provide a single **full-screen, modal overlay** that lists six power actions
   as large icons in a macOS-style grid (Lock · Restart · Exit · Hibernate ·
   Suspend · Shutdown), navigable by keyboard.
2. When a destructive action is selected, present a **pop-up confirmation
   dialog** (macOS-style) before invoking it.
3. Remain a **standalone binary mode** — invocable via `waylaunch --power`, bind
   it to a compositor keybind, identical deployment model to the switcher.
4. Add **zero new build-time dependencies.** Reuse cairo, pango, fontconfig,
   libxkbcommon, wayland-client. No GTK, no systemd-libs, no D-Bus.
5. **Minimum resource usage.** No long-lived worker threads, no resident daemon,
   no icon cache beyond the action icons already loaded from the system theme by
   `Renderer::draw_icon`. The overlay must mmap nothing extra and awaken on
   trigger rather than poll.

## 3. Non-goals

- Power-status monitoring (battery %, charging state) — out of scope; that is
  the compositor's panel / `waybar` territory.
- Scheduled or timer-based power events ("shutdown in 10 minutes").
- Login/auth prompts. Lock action delegates to `loginctl lock-session` (already
  a custom command example in `config/waylaunch.toml`).
- Replacing `systemd` or `elogind`. The manager issues one `loginctl` call per
  action and then exits.
- Multi-monitor independent placement. A single full-screen overlay covers the
  compositor's "active" output, matching how the switcher is rendered today.

---

## 4. Functional requirements

### 4.1 Actions

Six actions, all reachable from one overlay:

| ID | Display name | Default command | Destructive? | Default icon |
|----|--------------|-----------------|-------------|-------------|
| `lock`      | Lock     | `loginctl lock-session`              | No  | `system-lock-screen` |
| `restart`   | Restart  | `loginctl reboot`                   | Yes | `system-reboot` |
| `exit`      | Exit (logout) | `wayland-logout` *fallback `loginctl terminate-user $XDG_SESSION_ID`* | Yes | `system-log-out` |
| `hibernate` | Hibernate | `loginctl hibernate`                | Yes | `system-suspend-hibernate` |
| `suspend`   | Suspend  | `loginctl suspend`                 | Yes | `system-suspend` |
| `shutdown`  | Shut Down | `loginctl poweroff`                 | Yes | `system-shutdown` |

> `wayland-logout` is preferred for "Exit" on wlroots; if absent, fall back to
> `loginctl terminate-user "$XDG_SESSION_ID"`. The choice is made at action
   time, not hardcoded into config.

Every command **must be user-overridable** in `config/waylaunch.toml`.

### 4.2 Invocation

- `waylaunch --power` launches the overlay, grabs the keyboard via
  `wlr-layer-shell` (`keyboard_interactivity: exclusive`), and renders the
  grid centered.
- Single-instance guard `waylaunch-power.lock` (`src/main.cpp` pattern):
  a second invocation while one is running is a no-op — the running overlay
  is brought to focus and the new process exits 0. There is **no**
  signal-driven "advance" semantics (unlike the switcher); power overlay is
  not resident.
- Exit codes: `0` confirmed/cancelled normally, `1` init failure,
  `2` another instance already running.

### 4.3 Keyboard interaction

The overlay is keyboard-first. Keys must mirror the switcher's mental model so
users who learned Alt+Tab already know how to use it.

| Key | Action |
|-----|--------|
| `←` / `→` / `↑` / `↓` | Move selection (2-row grid wraps) |
| `Tab` / `Shift+Tab` | Next / previous action |
| `Home` / `End` | First / last action |
| `1` … `6` | Jump to action by index |
| `Return` / `Space` | Select (opens confirmation dialog if destructive; non-destructive runs immediately) |
| `Esc` | Cancel — close dialog if open, otherwise close overlay |
| `Ctrl+C` (sigint) | Same as Esc |

Non-destructive actions (Lock) execute immediately on `Return`. Destructive
actions show the confirmation dialog. The dialog is modal: it owns focus and
the only valid keys are `Return` (confirm), `Esc` / `Ctrl+C` (cancel). `Tab`
inside the dialog does **not** wrap to the underlying grid.

### 4.4 Confirmation dialog

- Centered **inside** the overlay as a child surface (not a separate
  `wl_surface`). It dims/blurs the grid behind it.
- macOS-flavoured visual language: rounded card, two buttons side-by-side —
  **Cancel** (subtle, accent-less) on the left, **action name in red** on the
  right for shutdown/restart/exit; **action name in normal accent** on the right
  for hibernate/suspend.
- Headline text: `"Are you sure you want to shut down your computer now?"`
- Subtext (optional): `"Open apps will be closed. Unsaved work will be lost."`
  for the four session-ending actions; omitted for hibernate/suspend.
- The dialog must be a **separate renderable component**, not baked into the
  grid renderer — to satisfy SRP and enable unit tests in isolation.

### 4.5 Visual design

- Reuse `Renderer::draw_backdrop` for the same frosted-glass look used by the
  launcher and switcher. **No new blur code.**
- Center full-screen tint with macOS-dark translucency (`0.1, 0.1, 0.14, ~0.85`),
  using the existing `Color::from_rgba` helper.
- Action grid: **2 rows × 3 columns**, large circular icons (~80px), centered
  icon symbols, subtle label under each.
- Selected item: accent-colored pill (`theme.accent` at alpha ~0.28 fill, ~0.6
  stroke) — exactly the same selection treatment as `SwitcherRenderer::render`
  in `src/switcher/switcher_renderer.cpp`.
- Confirmation dialog: a card ~420×220 with `corner_radius = 24`.
- Typography: reuse `theme.result_font` at `size = 18`, bold for headlines,
  normal for body. **No new font config keys.**

### 4.6 Configuration

Re-use the existing TOML loader. New section, additive only — no existing keys
change shape or default:

```toml
[power]
enabled_actions = ["lock", "restart", "exit", "hibernate", "suspend", "shutdown"]
confirm_destructive = true
font_scale = 1.0              # multiplier applied only inside the power overlay

[power.commands]             # override any default command
lock      = "loginctl lock-session"
restart   = "loginctl reboot"
exit      = "wayland-logout"
hibernate = "loginctl hibernate"
suspend   = "loginctl suspend"
shutdown  = "loginctl poweroff"

[power.confirm_text]         # optional localisation
restart   = "restart your computer"
exit      = "log out now"
hibernate = "hibernate your computer"
suspend   = "put your computer to sleep"
shutdown  = "shut down your computer now"
```

- Omitting `[power]` entirely yields full defaults. An empty
  `enabled_actions = []` disables the overlay (the binary prints a message and
  exits 0).
- Custom ordering follows list order in `enabled_actions`.
- All commands run through a single existing helper — `src/search/subprocess.cpp`
  — not `system()`. This avoids the security issue flagged in
  `docs/DESIGN.md` §1.3 and DRYs with the launcher's command mode.

### 4.7 Compositor integration

- Detect `wlr-layer-shell` via `WaylandCore`. If absent, fall back to a `xdg_toplevel`
  centered, always-on-top window — identical behaviour contract to the existing
  switcher fallback.
- On confirm, the overlay **destroys its Wayland surface first, then launches the
  command** to avoid the overlay staying visible during the action.
- A graceful 250ms destroy/hide animation is **not required** for v1; `hide()`
  is synchronous and the binary exits immediately after dispatch.

---

## 5. Architecture constraints (mandatory)

These are non-negotiable design rules derived from the project's stated
principles and from the critique in `docs/DESIGN.md`:

### 5.1 Module layout — mirror `switcher/`

```
include/waylaunch/power/
  power_action.h           # POD: id, name, command, icon, confirm_text, destructive
  power_action_backend.h   # IPowerActionBackend — pure interface, returns command/id mapping
  power_manager.h          # owns actions, selection, visibility (≈ AppSwitcherManager)
  power_state_machine.h    # tiny enum + transition fn (≈ SwitcherStateMachine)
  power_input_controller.h # key dispatch w/ reverse dispatch table (≈ SwitcherInputController)
  power_renderer.h         # draws grid + delegates dialog to ConfirmDialogRenderer
  confirm_dialog.h         # value object: shown?, action, callbacks
  confirm_dialog_renderer.h # separate renderer for the dialog card — SRP
src/power/
  power_action_backend.cpp # builds the 6 default actions, merges config overrides
  power_manager.cpp
  power_state_machine.cpp
  power_input_controller.cpp
  power_renderer.cpp
  confirm_dialog_renderer.cpp
```

No new code outside `src/power/` and `include/waylaunch/power/` may know about
power management — only `src/main.cpp` is modified to add `--power` and a new
mode flag on `LauncherUI` (or a small new entry shim — see §5.5).

### 5.2 Decoupling

- `PowerManager` depends on `IPowerActionBackend`, not on its concrete impl —
  same shape as `AppSwitcherManager : IToplevelObserver` /
  `IToplevelBackend`. This is the **Dependency Inversion Principle** applied
  identically to the switcher's `toplevel_backend.h`.
- `PowerRenderer` renders a `PowerManager` and (when open) a `ConfirmDialog`
  value object. It does **not** read input or assemble commands.
- `PowerInputController` depends on `PowerManager` + `PowerStateMachine` +
  `ConfirmDialog` only. It owns **no rendering knowledge** — symmetric to
  `SwitcherInputController`.
- `ConfirmDialogRenderer` is its own translation unit; the grid renderer calls
  it as a collaborator when `manager.confirm_dialog().is_open()`.

### 5.3 SOLID check (per module)

| Module | S | O | L | I | D |
|--------|---|---|---|---|---|
| PowerManager            | selection + visibility only | new actions via backend | —              | depends on `IPowerActionBackend` | — |
| PowerStateMachine       | enum transitions only       | new events = add enum   | one state graph | —                               | — |
| PowerInputController    | dispatch only               | key map extendable      | —              | —                               | — |
| PowerRenderer           | draws state given to it     | re-themable via Theme   | —              | —                               | — |
| ConfirmDialog / Renderer| isolated dialog value obj  | —                       | —              | —                               | — |
| PowerActionBackend      | builds default+overrides    | config-driven           | —              | implements `IPowerActionBackend` | — |

No god module. Each `.cpp` should stay under ~150 LOC. If a function exceeds
~30 LOC or cyclomatic complexity > 6, refactor before merge.

### 5.4 DRY / KISS

- **No re-implementation of**: subprocess spawning, blur, icon loading, color
  parsing, config loading, single-instance locking, layer-shell surface setup,
  keyboard grab. All come from `src/core/`, `src/ui/renderer.cpp`,
  `src/search/subprocess.cpp`, `src/config/`.
- The confirmation dialog UI is **one reusable component** — used by all five
  destructive actions. There is no per-action dialog code path.
- Selection pill rendering in `PowerRenderer` should reuse the exact formula in
  `SwitcherRenderer::render` (lines 66–73 of `src/switcher/switcher_renderer.cpp`).
  If the two diverge, factor a small `Renderer::draw_selection_pill(...)`
  helper **in `renderer.cpp`**, not in either feature dir — this DRYs the
  primitives without coupling the features.

### 5.5 Entry point

`src/main.cpp` gains a `--power` flag and a `power_mode` boolean, following the
exact pattern `--switch` already uses:

```cpp
// pseudocode — keep flow identical to switcher branch
bool power_mode = (...arg == "--power"); ...
int lock_fd = power_mode
    ? acquire_single_instance("waylaunch-power.lock", SIGTERM)
    : (switcher_mode ? ... : ...);
```

Either thread the mode through `LauncherUI` (which already has
`set_switcher_mode`) by adding `set_power_mode`, **or** extract a thin
`PowerOverlay::run(config)` entry analogous to the launcher — whichever yields
smaller diff and lower cyclomatic complexity in `main.cpp`. Recommendation:
extract; `main.cpp` is already crawling and a thin shim keeps the launcher
unchanged.

---

## 6. Performance and resource budget

| Metric | Target | Rationale |
|--------|--------|-----------|
| Cold-start latency (overlay visible) | < 120 ms on a warm compositor | matches existing switcher |
| Peak RSS over baseline (no overlay running) | < 800 KB extra | single-shot, no caches warmed |
| Resident after dismiss | 0 | binary exits; no daemon |
| Background CPU while shown | 0% | event-driven, no animation loop in v1 |
| Build time impact | < 5% | 6 small TUs, no new third-party headers |
| New runtime deps | 0 | `loginctl` is already in the project's stated command examples |
| New build-time deps | 0 | — |

Debug output via the existing `WAYLAUNCH_DEBUG` env flag — no new logging API.

---

## 7. Security

- Commands are executed via `src/search/subprocess.cpp` (fork+exec), never
  `system()`. Arguments are argv-split, not shell-joined. This is the fix
  recommended in `docs/DESIGN.md` §1.3 and is mandatory for the power overlay
  since it handles destructive commands.
- The overlay does not log the executed commands to `stdout`/`stderr` in
  non-debug mode.
- No session cookie, no D-Bus, no PolKit. The compositor already grants the
  user the right to call `loginctl` for their own session; we do not need extra
  authority.

---

## 8. Accessibility / i18n

- All visible strings come from config (`[power.confirm_text]` and action
  names). No hardcoded English in rendering paths.
- Keys are remappable by reusing existing `xkbcommon` mapping in
  `WaylandCore`. No new keymap code.
- A high-contrast option piggybacks on existing theme keys; no new
  `[power.theme]` section is introduced.

---

## 9. Testability

Following the precedent of `tests/app_switcher_test.cpp`:

1. `tests/power_manager_test.cpp` — selection navigation, jump-to, confirm
   gating on `destructive`, hide-on-cancel.
2. `tests/power_state_machine_test.cpp` — state transitions Hidden → Active →
   ConfirmOpen → Active | Dismissing.
3. `tests/power_action_backend_test.cpp` — default command set, override
   merging, disabled actions.
4. `tests/confirm_dialog_test.cpp` — open/close, action capture, reject when no
   action selected.

All tests must be header-only includable (no Wayland connection), mirroring the
existing switcher tests which instantiate `AppSwitcherManager` with a null
backend stub.

---

## 10. Acceptance criteria

- [ ] `waylaunch --power` opens a full-screen frosted-glass overlay showing six
      action icons in a 2×3 grid centered on the active output.
- [ ] Keyboard nav (arrows/Tab/Home/End/1–6) cycles the selection pill exactly
      as the switcher does.
- [ ] `Return` on `lock` runs the command immediately and the overlay destroys
      its surface + exits.
- [ ] `Return` on any destructive action opens the macOS-style confirmation
      dialog with the configurable headline + an appropriately-coloured
      confirm button.
- [ ] `Esc` at the grid closes the overlay. `Esc` inside the dialog returns to
      the grid.
- [ ] `Return` inside the dialog runs the action via `src/search/subprocess.cpp`
      and the binary exits after the next wayland roundtrip.
- [ ] A second `waylaunch --power` invocation while the first is showing exits
      cleanly with code 2 (or 0 if we decide "no-op") and the visible overlay is
      brought to front.
- [ ] `[power]` config block is fully optional; defaults reproduce the section
      §4.1 table.
- [ ] `npm run lint` / `cmake --build build` and the existing test target
      continue to pass with no new warnings.
- [ ] No new third-party library is added to `CMakeLists.txt`; the only build
      diff is `target_sources(... src/power/*.cpp)` and the new test files.
- [ ] `ldd build/waylaunch` shows no new shared libraries.
- [ ] `docs/README.md` gains a "Power actions" section mirroring the existing
      "App switcher" section.

---

## 11. Out of scope for v1 (parking lot)

- Repeating "Suspend for 1 hour" / scheduled wake.
- Battery / UPS awareness.
- User-switching (Fast User Switch).
- Per-action confirmation skipping beyond the
  `confirm_destructive = false` global toggle.
- Mouse / touch interaction — keyboard only for v1, matching switcher.
- A single unified keybind dispatcher across launcher / switcher / power — each
  overlay owns its own dispatch table today; unifying is a separate refactor.
- Integration with the custom `[commands]` system in the launcher. The custom
  command mode is already user-defined and fully capable of issuing `loginctl`
  calls; the power overlay is its purpose-built sibling, not a replacement.

---

## 12. Open questions

1. Should `Exit` prefer `wayland-logout`, `loginctl terminate-user`, or a
   user-supplied command in `[power.commands.exit]` — and what is the discovery
   order at action time?
2. Should the confirmation dialog offer a "Don't ask again for this action"
   checkbox (per-action suppression)? Parked for v2 unless demanded now.
3. Should a running power overlay be dismissible by an arbitrary global key
   (e.g. Super+D opens the launcher while power overlay is up)? Current
   contract: only Esc / Ctrl+C dismiss; the launcher's lock is independent so
   the launcher can stack over it — but its keyboard grab will fail until the
   power overlay's grab is released. Decision: leave the grab; document.
4. Does `waylaunch --power` belong behind the same `--switch`-style resident
   mechanism, or is one-shot strictly better for a destructive UI? Default:
   one-shot — matches macOS dialog semantics where the panel is created and
   destroyed per invocation, not kept alive.
