# waylaunch ↔ qypr — Integration Analysis & Staged Plan

> **Scope:** whether the two repositories `ngarana/waylaunch` (this one) and
> `ngarana/lockscreen` (qypr) should be consolidated into one project, what that
> would buy, what it would cost, and — if yes — in what order.
>
> **Companion documents:** [`DESIGN.md`](DESIGN.md) (waylaunch architecture),
> [`POWER_MANAGER.md`](POWER_MANAGER.md) (the overlay this document proposes
> qypr adopt), and qypr's `docs/ROADMAP.md` / `docs/STATUS_BAR.md`.
>
> **Last updated:** 2026-08-25. Status: **analysis complete, decision open.**
> §9 lists what the maintainer still has to settle.

---

## 1. Executive summary

The two projects are not independent codebases that *could* be integrated. They
are **already one desktop suite at runtime** — six binaries installed side by
side in `/usr/local/bin`, bound together in one Hyprland config, four of them
resident at any moment — that happens to be maintained as two repositories with
duplicated seams.

The measured overlap is roughly **4,000 lines in qypr and 2,800 lines in
waylaunch addressing the same concerns**, of which a realistic consolidation
recovers **1,500–2,500 net LOC**. Two features are duplicated all the way to the
user: the **power menu** (both implementations are shipped *and both are bound
in the live config*) and the **application launcher** (qypr ships a 214-line
in-bar launcher; waylaunch is a launcher).

The overlap is also **growing**: qypr's ROADMAP Phase 15 plans "launcher,
clipboard, keyboard layout, idle inhibitor, system monitors" as utility
indicators — four of the five are things waylaunch either does or is.

But a full merge is blocked by something real, not stylistic: **the two projects
have contradictory architectural constitutions.** qypr's ROADMAP lists as a
non-negotiable gate "no spawned processes, no polling, one shared bus connection
per bus, **no threads**". waylaunch runs a worker thread, forks sandboxed
extractor subprocesses, and ships a resident indexing daemon. That gate is not
decoration — it exists to keep a PAM-authenticating lock surface small and
auditable.

**Recommendation (§8): stage the consolidation.** Deduplicate the two
*user-visible* features first (days, near-zero risk, no architectural
commitment), extract a shared core library second (weeks, bounded churn), and
treat the full monorepo merge as a separate decision that depends on relaxing
qypr's threading gate — which should be settled deliberately, not as merge
fallout.

---

## 2. What exists today (as-is)

### 2.1 The two repositories

| | qypr | waylaunch |
|---|---|---|
| Remote | `github.com/ngarana/lockscreen` | `github.com/ngarana/waylaunch` |
| Source LOC | 24,032 | 11,818 (`src` + `include`) |
| Test LOC | 3,604 | 3,247 |
| Commits | 131 | 50 |
| Active span | 2026-04-08 → 2026-08-24 | 2026-07-18 → 2026-08-16 |
| Working branch | `qol` (dirty) | `feat/spotlight-ui-refinement` |
| Language | C++20, CMake ≥ 3.20 | C++20, CMake ≥ 3.20 |
| Rendering | cairo + pangocairo + `wl_shm`, no GPU | identical |
| Namespace | `qypr` | `waylaunch` |
| Header ext. | `.hpp` | `.h` |
| Naming | `PascalCase.hpp`, `camelCase()` | `snake_case.h`, `snake_case()` |
| Config format | hand-rolled INI + `import` (decision D2: no new dependency) | TOML via `toml++` |
| `.clang-format` | present | **absent** |
| Event loop | epoll reactor, `core/EventLoop` (133 L) | `poll` + `eventfd`, inline in `launcher_ui` |

The repository name `lockscreen` is now inaccurate — qypr ships a full
KDE-Plasma-calibre panel replacement alongside the locker.

### 2.2 The runtime picture — already one suite

From the live Hyprland config (`~/.config/hypr/modules/binds.lua`,
`autostart.lua`, `hypridle.conf`):

```
Super+D        → waylaunch                    (launcher)
Alt+Tab        → waylaunch --switcher         (resident overlay)
Alt+Shift+Tab  → waylaunch --switcher --reverse
Super+Escape   → waylaunch --power            (power overlay)
Super+L        → ~/.config/qypr/lock.sh       (locker)
hypridle       → qypr-lock --idle-timeout 30
autostart      → qypr-bar                     (panel)
systemd --user → qypr-notification-log.service (qypr-record)
systemd --user → waylaunchd.service            (content index)
```

Four processes resident, **130.1 MB RSS total**:

| Process | RSS |
|---|---|
| `qypr-bar` | 68.5 MB |
| `waylaunchd` | 35.0 MB |
| `waylaunch --switcher` | 21.7 MB |
| `qypr-record` | 5.0 MB |

Installed binary sizes: `qypr-lock` 2.0 MB, `qypr-bar` 1.8 MB, `qypr-record`
0.16 MB; `waylaunch` **17.2 MB**, `waylaunchd` 5.6 MB, `waylaunchctl` 2.5 MB.

### 2.3 Shared foundations (the reason integration is even plausible)

Both are hand-rolled Wayland clients over the same stack — `wayland-client`,
`wayland-cursor`, `xkbcommon`, `cairo`, `pangocairo`, `librsvg-2.0` — with
software `wl_shm` rendering and no GTK/Qt/EGL anywhere. Both target
wlroots-class compositors via `wlr-layer-shell`. Both use the same
Catppuccin Mocha palette, down to identical hex values:

```
#1e1e2e  #313244  #45475a  #89b4fa  #cdd6f4  #6c7086  #f38ba8  #a6e3a1
```

There is **no shared source of truth** for that palette — qypr reads
`themes/catppuccin-mocha.conf`, waylaunch defaults them in
`ColorConfig` (`include/waylaunch/config.h`). They match because they were kept
in sync by hand, and the sync has **already slipped**: `warning` is `#f9e2af`
in qypr and `#fab387` in waylaunch.

---

## 3. The overlap (measured)

### 3.1 Duplicated concerns

| Concern | qypr | waylaunch |
|---|---|---|
| `.desktop` index + search | `system/DesktopIndex` — 312 L | `modes/app_launcher` — 167 L |
| XDG icon resolve + cairo cache | `ui/IconResolver` — 497 L | icon cache in `ui/renderer.cpp` |
| Cairo/Pango draw helpers | `render/Painter` — 345 L | part of `ui/renderer.cpp` — 631 L |
| Layer-shell / shm / seat / xkb / output | `src/wayland/` — 1,735 L | `core/wayland_core.cpp` — 660 L |
| `wlr-foreign-toplevel` client | `system/ToplevelBackend` — 347 L | `switcher/wlr_toplevel_backend` — 276 L |
| Power actions | `power/PowerManager` + `ui/PowerDialog` + `PowerMenuIndicator` — 574 L | `src/power/` — 873 L |
| Application launcher UI | `ui/statusbar/LauncherPopover` — 214 L | the product |
| Event loop | `core/EventLoop` — 133 L | inline `poll`/`eventfd` |
| Config loader | `core/Config` — 181 L | `config/config.cpp` — 338 L |

Both `.desktop` implementations parse a `DesktopEntry`, strip `Exec` field
codes, rank prefix-then-substring, and spawn detached. Both icon paths walk the
freedesktop theme inheritance chain, load PNG through cairo and SVG through
librsvg, and cache surfaces.

Note that the concern totals are **not** all recoverable. Each side has
capability the other lacks — waylaunch's `wlr-screencopy` backdrop blur, qypr's
multi-output handling and `ext-session-lock-v1` session. The recoverable figure
is the 1,500–2,500 LOC in §1.

### 3.2 Duplicated *features* — visible to the user, today

**Two power menus are live simultaneously.** `power` appears in
`modules-center` of `~/.config/qypr/bar.conf` (line 73), *and* Super+Escape is
bound to `waylaunch --power`. They differ in substance, not just skin:

| | qypr `PowerDialog` | waylaunch `--power` |
|---|---|---|
| Command set | hardcoded `systemctl` verbs | config-driven, argv-split, no shell |
| Init system | systemd assumed | normalized systemd/elogind at action time |
| Confirmation | anchored popover, countdown → **auto-cancel** | standalone card, countdown → **auto-confirm** |
| Configurability | none | `enabled_actions`, `commands`, `confirm_text`, `countdown_seconds` |
| Tests | none dedicated | 5 test binaries |

The countdown semantics are **opposite** — qypr's expiry cancels, waylaunch's
expiry executes. That is a genuine behavioural inconsistency in one desktop.

**Two application launchers exist.** qypr's `LauncherIndicator` +
`LauncherPopover` is a keyboard-driven `.desktop` search over `DesktopIndex`.
It is built into `qypr-bar` but **not enabled in the live `bar.conf`** — so it
is currently 214 lines of compiled, untested-in-practice duplication of the
thing Super+D already does better.

### 3.3 Divergent vendored protocol XML

Both repositories vendor `protocols/` independently, and the copies differ:

| Protocol | qypr | waylaunch |
|---|---|---|
| `wlr-layer-shell-unstable-v1` | 407 L — full upstream | **151 L — hand-reduced** |
| `wlr-foreign-toplevel-management-unstable-v1` | 270 L | 267 L (whitespace/comment drift) |
| `wlr-screencopy-unstable-v1` | — | 135 L (blur) |
| `wlr-gamma-control-unstable-v1` | 126 L (night light) | — |

waylaunch's layer-shell XML is missing three requests present upstream and in
qypr's copy: `zwlr_layer_shell_v1.destroy`, `zwlr_layer_surface_v1.set_layer`,
and `set_exclusive_edge`. This file has caused a protocol bug in this repo
before. **One canonical `protocols/` directory eliminates the entire bug class**
— and a merged suite needs the union of all four files anyway.

---

## 4. Divergences that resist merging

### 4.1 Contradictory architectural constitutions — the real blocker

qypr's ROADMAP, "Non-negotiable gates (every phase)", gate 2:

> **Minimal footprint** — no spawned processes, no polling (push via fds in the
> epoll loop), one shared bus connection per bus, no threads.

waylaunch, in the live path:

- a **worker thread** for the async filesystem walk (`FileWorkerLoop`, woken via
  `eventfd`),
- **forked subprocesses** for content extraction (`pdftotext`, `unzip`,
  `pandoc`, `odt2txt`) under a cgroup sandbox,
- a **resident indexing daemon** (`waylaunchd`) with inotify watches and a
  periodic reconcile.

These are not reconcilable by style guide. qypr's gate exists because
`qypr-lock` authenticates with PAM and must stay small and auditable; waylaunch's
thread and subprocesses exist because full-text indexing cannot be done on a UI
thread. **A merge requires one side's rule to lose**, and that is a design
decision the maintainer has to make explicitly.

### 4.2 Dependency and attack surface

Merged link sets, if the binaries were unified:

- qypr contributes `libpam`, `mpv`, `libpulse`, `libudev`, `sdbus-c++`,
  `libsystemd`.
- waylaunch contributes `sqlite3` (FTS5), `libzstd`, `libmagic`, `toml++`.

Either direction is unattractive. A document indexer reachable from the lock
screen's process image, or PAM linked into a 17 MB launcher, both enlarge a
blast radius that is currently well separated. The size asymmetry is
instructive: `qypr-lock` is 2.0 MB; `waylaunch` is 17.2 MB.

**This argues for keeping distinct binaries even under a single repository** —
which is fine, and is what qypr already does across its own three.

### 4.3 Conventions

`namespace qypr` / `PascalCase.hpp` / `camelCase()` / hand-rolled INI versus
`namespace waylaunch` / `snake_case.h` / `snake_case()` / `toml++`, with no
`.clang-format` on the waylaunch side. A wholesale unification is a mass rename
across ~36,000 lines that destroys `git blame` continuity in both histories.

Note qypr's INI is an explicit decision (STATUS_BAR.md D2: "no new dependency"),
so "just move everything to TOML" reverses a recorded decision rather than
filling a gap.

### 4.4 Timing

qypr has uncommitted work on `qol` (`WifiBackend`, `QSTile`,
`QuickSettingsPanel`, `PopoverManager`, `DetailedPopover`) and 131 commits of
momentum. waylaunch sits on `feat/spotlight-ui-refinement` with four live
feature branches. A structural merge freezes or rebases both.

---

## 5. Integration options (with rejected alternatives)

| Option | What it is | Verdict |
|---|---|---|
| **A. Full monorepo merge** | One repo, one namespace, shared core, N binaries | **Deferred** — blocked on §4.1; mass rename cost in §4.3 |
| **B. Shared core library** | Extract `libwl-common`; both repos consume it (subtree/submodule) | **Recommended, Stage 2** — bounded churn, no constitutional conflict |
| **C. Runtime feature dedup** | Keep both repos; delete duplicate *features*; cross-exec | **Recommended, Stage 1** — days, near-zero risk |
| **D. Status quo** | Nothing | **Rejected** — duplication is growing (ROADMAP Phase 15), and the palette has already drifted |

Options B and C compose: C removes the user-visible inconsistency immediately
without committing to any code-sharing mechanism; B removes the maintenance
cost afterwards. Neither forecloses A.

---

## 6. Pros of consolidating

1. **Removes ~1,500–2,500 net LOC** of genuine duplication, plus one of the two
   power UIs and one of the two launchers outright.
2. **One theme, one config, one look.** A palette change becomes one edit
   instead of a two-repo hand-sync that has already drifted (§2.3).
3. **Process consolidation is available.** `qypr-bar` is already a resident
   layer-shell client owning an epoll loop, a seat, an icon resolver, and a
   toplevel backend — exactly the set `waylaunch --switcher` keeps resident for
   21.7 MB. Folding the switcher into the bar plausibly recovers ~20 MB and
   removes a lock file, a SIGUSR1/2 protocol, and the single-instance dance.
4. **Cross-features unlock.** waylaunch's content index could back a qypr-bar
   search popover; qypr's MPRIS/battery/WiFi/Bluetooth backends could become
   waylaunch `ResultProvider`s; the bar's launcher button could open the real
   Spotlight rather than its 214-line stand-in.
5. **One CI and one test convention.** 3.6k + 3.2k lines of tests currently sit
   in two different harnesses.
6. **Protocol vendoring stops being a bug source** (§3.3).
7. **Naming honesty** — a consolidation is the natural moment to retire the
   `lockscreen` repository name.

## 7. Cons and risks

1. **The threading/spawning gate conflict (§4.1)** — the blocker, and a design
   decision rather than an integration detail.
2. **Attack- and dependency-surface growth (§4.2)** if binaries are unified;
   mitigated by keeping separate binaries even in one repo.
3. **Convention churn destroys `git blame` (§4.3)** across both histories.
4. **Bad timing against two active branches (§4.4).**
5. **Coupled release cadence.** Today a launcher regression cannot break the
   lock screen. After a shared `Painter`, it can. This is the single strongest
   argument for keeping `qypr-lock` isolated regardless of what else merges.
6. **Loss of independent bisectability** across the two histories.

---

## 8. Recommendation — staged consolidation

### Stage 1 — deduplicate *features*, not code (days, near-zero risk)

No shared code, no build changes, no architectural commitment. Purely removes
the user-visible inconsistency.

- [ ] Point qypr's `PowerMenuIndicator` at `waylaunch --power` (spawn on click)
      and **delete `ui/PowerDialog`** (≈ 320 L). Resolves the opposite-countdown
      inconsistency in §3.2 in favour of the tested, configurable implementation.
- [ ] Point qypr's `LauncherIndicator` at `waylaunch` and **delete
      `ui/statusbar/LauncherPopover`** (≈ 214 L).
- [ ] Copy qypr's full 407-line `wlr-layer-shell-unstable-v1.xml` over
      waylaunch's hand-reduced 151-line copy; re-run `wayland-scanner`; confirm
      no regression in the launcher, switcher, and power overlays.
- [ ] Reconcile the one drifted palette value (`warning`: `#f9e2af` vs
      `#fab387`) and record which is canonical.
- [ ] Document the `--switch` / `--switcher` / `--command-tab` aliases in the
      waylaunch README (`src/main.cpp:66` accepts all three; the live
      `binds.lua:126` uses `--switcher`, which the README never mentions).

**Net:** ≈ 535 LOC deleted (`PowerDialog` 320 + `LauncherPopover` 214), both duplicate UIs gone, one protocol bug class
closed. Neither architecture is touched.

### Stage 2 — extract a shared core, keep two repositories (weeks)

A `libwl-common` consumed by both as a git subtree (preferred over a submodule:
no detached-HEAD friction, vendored builds stay reproducible).

Extraction candidates, in dependency order:

1. `protocols/` — the union of all four XML files. **Do this one first**; it is
   pure data and validates the mechanism at zero risk.
2. `EventLoop` — take qypr's epoll reactor; it already has `addFd`/`addTimer`/
   `post`/`addPrepare` and is the more general of the two.
3. `Painter` — qypr's cairo/pango helpers, extended with waylaunch's
   screencopy-backed blur.
4. `IconResolver` — qypr's (497 L, theme-inheritance aware, bounded LRU) is the
   more complete implementation.
5. `DesktopIndex` — qypr's, plus waylaunch's precomputed `search_key`
   optimisation (one `find()` per entry per keystroke instead of re-lowercasing
   four fields).
6. `ShmBuffer` + layer-shell surface setup.
7. `ToplevelBackend` — behind waylaunch's existing `IToplevelBackend` seam,
   which already exists for exactly this reason.

Adopt qypr's `.clang-format` and naming **for the shared library only**, so the
convention churn is bounded to the extracted files and neither product's history
is rewritten wholesale. Both sides already write to interfaces
(`qypr/core/Interfaces.hpp`; waylaunch's `IToplevelBackend` /
`IPowerActionBackend` / `ResultProvider`), so the seams exist.

### Stage 3 — fold the switcher into `qypr-bar` (optional)

The largest single runtime win (~20 MB, one fewer resident process). `qypr-bar`
already owns every dependency the switcher needs. Requires Stage 2's
`ToplevelBackend` to be shared first.

### Explicitly not doing (for now)

- **Option A, the full monorepo merge**, until §4.1 is settled.
- **Unifying the config format.** INI in qypr, TOML in waylaunch. Sharing a
  *palette file* (Stage 1) does not require sharing a *parser*.
- **Linking waylaunch's indexer into any qypr binary** (§4.2).

---

## 9. Open questions for the maintainer

1. **Does qypr's no-threads / no-spawn gate apply to the whole suite, or only
   to `qypr-lock`?** This single answer determines whether Option A is ever
   viable. A defensible split: the gate binds `qypr-lock` (a PAM surface)
   absolutely, and binds `qypr-bar` by default with documented exceptions.
2. **One repository or two?** Stage 2 works either way — a subtree consumed by
   two repos, or one repo with a `common/` directory. Two repos preserve
   independent release cadence (§7.5); one repo removes the sync tax.
3. **Which name survives?** `qypr` is the broader brand (a desktop suite);
   `waylaunch` names one component. The `lockscreen` repo name should retire
   regardless.
4. **Is the ~20 MB from Stage 3 worth coupling the switcher's lifetime to the
   bar's?** A bar crash would currently take Alt+Tab with it.

---

## Appendix A — measurement method

Every figure in this document was measured on 2026-08-25 against
`waylaunch@feat/spotlight-ui-refinement` and `qypr@qol`.

```sh
# Source and test LOC (per repo)
find src include -name '*.cpp' -o -name '*.hpp' -o -name '*.h' | xargs wc -l | tail -1
find tests -name '*.cpp' -o -name '*.h' | xargs wc -l | tail -1

# History
git log --oneline | wc -l
git log -1 --format='%ci'; git log --format='%ci' | tail -1

# Resident footprint of the live suite
ps -eo rss,comm | grep -iE 'qypr|waylaunch' | grep -v grep \
  | awk '{s+=$1; printf "%-14s %6.1f MB\n", $2, $1/1024} END {printf "TOTAL %.1f MB\n", s/1024}'

# Protocol XML divergence
diff qypr/protocols/wlr-layer-shell-unstable-v1.xml \
     waylaunch/protocols/wlr-layer-shell-unstable-v1.xml
grep -oE '<(request|event|interface) name="[a-z_0-9]+"' <file>   # request inventory

# Live desktop wiring
grep -rn 'qypr\|waylaunch' ~/.config/hypr/
```
