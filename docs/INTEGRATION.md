# waylaunch ↔ qypr — Integration Analysis & Staged Plan

> **Scope:** whether the two repositories `ngarana/waylaunch` (this one) and
> `ngarana/lockscreen` (qypr) should be consolidated into one project, what that
> would buy, what it would cost, and — if yes — in what order.
>
> **Companion documents:** [`DESIGN.md`](DESIGN.md) (waylaunch architecture),
> [`POWER_MANAGER.md`](POWER_MANAGER.md) (the overlay qypr's bar already
> delegates to), qypr's `docs/LOCK_SECURITY_REVIEW.md` (the lock-screen
> invariants this revision relies on), and qypr's `docs/ROADMAP.md` /
> `docs/STATUS_BAR.md`.
>
> **Last updated:** 2026-09-15 (revision 2). Status: **analysis complete,
> decision open.** §9 lists what the maintainer still has to settle.
>
> **Revision 2 corrects three errors in the 2026-08-25 version:**
> 1. It called qypr's no-threads/no-spawn gate a lock-screen security rule and
>    "the real blocker". It is a footprint rule, and it blocks nothing (§4.1).
> 2. It said two power menus were live on the desktop. That was already false
>    when written — the bar has delegated to `waylaunch --power` since
>    2026-07-25 (§3.2).
> 3. Stage 1 proposed deleting qypr's `ui/PowerDialog`. That is the **lock
>    screen's** power confirmation and must stay (§3.2, §8).
>
> **Precondition.** This revision assumes the remediations for findings QL-1 …
> QL-7 in qypr's `docs/LOCK_SECURITY_REVIEW.md` have landed. The invariants in
> §4.1 are stated as facts on that basis; until those fixes merge, they are
> targets.
>
> Figures in §2–§3 were measured on 2026-08-25 unless marked
> *re-verified 2026-09-15*.

---

## 1. Executive summary

The two projects are not independent codebases that *could* be integrated. They
are **already one desktop suite at runtime** — six binaries installed side by
side in `/usr/local/bin`, bound together in one Hyprland config, four of them
resident at any moment — that happens to be maintained as two repositories with
duplicated seams.

The measured overlap is roughly **4,000 lines in qypr and 2,800 lines in
waylaunch addressing the same concerns**, of which a realistic consolidation
recovers **1,500–2,500 net LOC**. One feature is still duplicated in code: the
**application launcher** (qypr ships a 228-line in-bar launcher, shelved and off
by default; waylaunch is a launcher). The **power menu is not** — the bar
already hands off to `waylaunch --power`; what is left in qypr is 102 lines of
unreachable bar popover, plus the lock screen's own dialog, which has to stay.

The overlap is also **growing**: qypr's ROADMAP Phase 15 plans "launcher,
clipboard, keyboard layout, idle inhibitor, system monitors" as utility
indicators — four of the five are things waylaunch either does or is.

The first version of this document said a full merge was blocked by qypr's
"no threads, no spawned processes" gate. **It isn't.** That gate is a resource
rule written for the status bar; qypr's own decisions D1 and D4 already narrow
it; and `qypr-lock` has always used a PAM worker thread and forked `systemctl`.
What actually protects the lock screen is a set of **security invariants** —
default-deny interactivity, a wiped and non-dumpable password buffer, a single
hardened spawn path, and a lock binary that never links the indexer (§4.1). None
of them forbids threads or subprocesses in *other* binaries. They constrain how
shared code may be hosted by `qypr-lock`, and each one is testable.

**Recommendation (§8): stage the consolidation.** Remove the remaining
duplicate UI code first (days). Extract a shared core second (weeks) — now
including a hardened spawn primitive both projects need. A full monorepo merge is
**no longer blocked**; it becomes an optional Stage 4, decided on convention
churn and release coupling, and gated on the §4.1 invariants running in CI.

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
| Working branch *(re-verified 2026-09-15)* | `qol@27d7de1` | `feat/dropdown-host@485d9f0` |
| Language | C++20, CMake ≥ 3.20 | C++20, CMake ≥ 3.20 |
| Rendering | cairo + pangocairo + `wl_shm`, no GPU | identical |
| Namespace | `qypr` | `waylaunch` |
| Header ext. | `.hpp` | `.h` |
| Naming | `PascalCase.hpp`, `camelCase()` | `snake_case.h`, `snake_case()` |
| Config format | hand-rolled INI + `import` (decision D2: no new dependency) | TOML via `toml++` |
| `.clang-format` *(re-verified 2026-09-15)* | present | present, enforced with clang-tidy since `f0a36fc` — **different options** from qypr's |
| Event loop | epoll reactor, `core/EventLoop` (133 L) | `poll` + `eventfd`, inline in `launcher_ui` |

The repository name `lockscreen` is now inaccurate — qypr ships a full
KDE-Plasma-calibre panel replacement alongside the locker.

### 2.2 The runtime picture — already one suite

From the live Hyprland config (`~/.config/hypr/modules/binds.lua`,
`autostart.lua`, `hypridle.conf`) and qypr's `bar.conf`:

```
Super+D        → waylaunch                    (launcher)
Alt+Tab        → waylaunch --switcher         (resident overlay)
Alt+Shift+Tab  → waylaunch --switcher --reverse
Super+Escape   → waylaunch --power            (power overlay)
qypr-bar power → Quick Settings → waylaunch --power
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
in sync by hand, and the sync has **already slipped** *(re-verified
2026-09-15)*: `warning` is `#f9e2af` in qypr and `#fab387` in waylaunch.

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
| Power actions (desktop) | `power/PowerManager` — 62 L + bar `PowerMenuPopover` — 102 L, unreachable | `src/power/` — 873 L |
| Subprocess spawning | three `fork` call sites (`PowerManager`, `QuickSettingsPanel`, `DesktopIndex`) | `search/subprocess.cpp` — `posix_spawn`, 238 L |
| Application launcher UI | `ui/statusbar/LauncherPopover` — 228 L *(re-verified 2026-09-15)* | the product |
| Event loop | `core/EventLoop` — 133 L | inline `poll`/`eventfd` |
| Config loader | `core/Config` — 181 L | `config/config.cpp` — 338 L |

qypr's `ui/PowerDialog` (320 L) is **not** in this table. It is lock-only
(`QYPR_LOCK_ONLY_SOURCES`) and nothing in waylaunch can replace it (§3.2).

Both `.desktop` implementations parse a `DesktopEntry`, strip `Exec` field
codes, rank prefix-then-substring, and spawn detached. Both icon paths walk the
freedesktop theme inheritance chain, load PNG through cairo and SVG through
librsvg, and cache surfaces.

Note that the concern totals are **not** all recoverable. Each side has
capability the other lacks — waylaunch's `wlr-screencopy` backdrop blur, qypr's
multi-output handling and `ext-session-lock-v1` session. The recoverable figure
is the 1,500–2,500 LOC in §1.

### 3.2 Duplicated *features* — visible to the user

**The power menu is not duplicated on the desktop.** The first version of this
document said it was; that was already false when it was written. Since qypr
commit `530998d` (2026-07-25), activating the bar's `power` indicator always
opens Quick Settings (`StatusBar.cpp:565`), and the Quick Settings power tile runs
`waylaunch --power` (`QuickSettingsPanel.cpp:103`, overridable through
`[quick-settings] power-command`, which the live `bar.conf` leaves commented
out). The desktop has **one** power UI — waylaunch's — reachable from
Super+Escape and from the bar.

Two pieces remain in qypr:

| Code | Status | Action |
|---|---|---|
| Bar `PowerMenuPopover` (`PowerMenuIndicator.cpp:39-140`, 102 L) | **Unreachable** — `activateIndicator` diverts `power` to Quick Settings, and the tile ignores its `onPower` callback | Delete (Stage 1) |
| Lock `ui/PowerDialog` (320 L) | Lock-only | **Keep** |

**The lock screen's `PowerDialog` must stay**, for two reasons:
- A layer-shell overlay cannot draw above an `ext-session-lock-v1` surface.
- Launching another binary from the lock screen before authentication is exactly
  what invariant I1 forbids (§4.1).

Its countdown **cancels** on expiry, while waylaunch's **confirms**. That
difference is correct, not an inconsistency: before authentication the safe
default is to do nothing, and on an unlocked desktop the user has already
chosen the action.

**Two application launchers exist.** qypr's `LauncherIndicator` +
`LauncherPopover` is a keyboard-driven `.desktop` search over `DesktopIndex`.
qypr's `STATUS_BAR.md` marks it *shelved* — built, tested, off by default — and
it is not enabled in the live `bar.conf`. It is bar-only; the lock app never
constructs a `DesktopIndex`.

### 3.3 Divergent vendored protocol XML

Both repositories vendor `protocols/` independently, and the copies differ
*(re-verified 2026-09-15: unchanged)*:

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

## 4. Divergences — and what actually constrains a merge

### 4.1 The threading/spawning gate is not a blocker

qypr's ROADMAP, "Non-negotiable gates (every phase)", gate 2:

> **Minimal footprint** — no spawned processes, no polling (push via fds in the
> epoll loop), one shared bus connection per bus, no threads.

waylaunch, meanwhile, runs a **worker thread** for the async filesystem walk,
**forks sandboxed extractor subprocesses** (`pdftotext`, `unzip`, `pandoc`,
`odt2txt`), and ships a **resident indexing daemon** (`waylaunchd`).

The first version of this document called that "the real blocker" and claimed
the gate "exists to keep a PAM-authenticating lock surface small and
auditable". Neither holds:

- **It is a footprint rule.** The gate is titled *Minimal footprint* and lives
  in the status-bar roadmap.
- **qypr's own decisions narrow it.** `STATUS_BAR.md` D1: "the codebase's real
  rule is narrower than the prose" — spawning *to read state* is banned;
  user-initiated one-shot launches are allowed. D4: polling is "tolerable on the
  lock screen but not for an always-running panel" — the gate is *looser* for
  the locker, not stricter.
- **`qypr-lock` never followed it literally.** It has always authenticated on a
  worker thread (`PamAuthenticator.cpp:69`) and forked `systemctl`
  (`PowerManager.cpp:22`). A non-root PAM locker cannot avoid a subprocess
  anyway: `pam_unix` runs the setuid `unix_chkpwd` on every password attempt.

**What protects the lock screen is a set of invariants**, which qypr's security
review made explicit. With its remediations in place:

| Invariant | Established by | What it means for shared code |
|---|---|---|
| **I1 — Default-deny interactivity.** Before authentication, an indicator can act only if it explicitly opts in | QL-1, QL-2, QL-4, QL-7 | Any widget or indicator hosted by `qypr-lock` goes through the policy hook. A shared component cannot add pre-authentication surface just by existing |
| **I2 — The secret is contained.** The password lives only in a `SecureBuffer` (fixed capacity, `mlock`ed, zeroed on every release), and the process is non-dumpable | QL-3 | Shared input and text code never holds the secret, and nothing linked into `qypr-lock` may copy lock-path input into an ordinary string or re-enable dumping |
| **I3 — One spawn path.** Subprocesses start only through `posix_spawn` with absolute paths and default signal dispositions, reaped by `pidfd` in the event loop. No process-wide signal dispositions | QL-5, QL-6 | A shared spawn primitive is a *requirement*, not a conflict — and waylaunch's `Subprocess` wrapper already uses `posix_spawn` |
| **I4 — The lock never links the indexer.** `qypr-lock` does not link `waylaunch_content`, the extractors or the launcher's search providers | §4.2 | A build-graph rule, checkable in CMake |

waylaunch's worker thread, extractor subprocesses and `waylaunchd` break none of
these: they live in other binaries (I4), and its spawn code is already the
`posix_spawn` model I3 requires. **The conflict the first version described does
not exist at the binary level.** What a merge has to preserve is I1–I4 — and
because the security review gives a failing test for each finding, they can be
enforced as CI gates rather than conventions.

### 4.2 Dependency and attack surface

Merged link sets, if the binaries were unified:

- qypr contributes `libpam`, `mpv`, `libpulse`, `libudev`, `sdbus-c++`,
  `libsystemd`.
- waylaunch contributes `sqlite3` (FTS5), `libzstd`, `libmagic`, `toml++`.

Either direction is unattractive. A document indexer reachable from the lock
screen's process image, or PAM linked into a 17 MB launcher, both enlarge a
blast radius that is currently well separated. The size asymmetry is
instructive: `qypr-lock` is 2.0 MB; `waylaunch` is 17.2 MB.

**Keep distinct binaries even under a single repository** — which is what qypr
already does across its own three. Invariant I4 turns that preference into a
build rule.

### 4.3 Conventions

`namespace qypr` / `PascalCase.hpp` / `camelCase()` / hand-rolled INI versus
`namespace waylaunch` / `snake_case.h` / `snake_case()` / `toml++`. waylaunch now
has a `.clang-format` enforced repo-wide (`f0a36fc`, 2026-09-05), but its
options differ from qypr's, so both formatting and naming still diverge. A
wholesale unification is a mass rename across ~36,000 lines that destroys
`git blame` continuity in both histories.

Note qypr's INI is an explicit decision (STATUS_BAR.md D2: "no new dependency"),
so "just move everything to TOML" reverses a recorded decision rather than
filling a gap.

### 4.4 Timing

*(Re-verified 2026-09-15.)* Both repositories are active: qypr on
`qol@27d7de1` with five local branches, waylaunch on
`feat/dropdown-host@485d9f0` with six and uncommitted work in progress. Stages 1
and 2 touch few files and tolerate this. A structural merge (Stage 4) still
freezes or rebases both.

---

## 5. Integration options (with rejected alternatives)

| Option | What it is | Verdict |
|---|---|---|
| **A. Full monorepo merge** | One repo, shared core, separate binaries | **Viable, not yet recommended** — no longer blocked (§4.1). The cost is convention churn (§4.3) and release coupling (§7). Optional Stage 4 |
| **B. Shared core library** | Extract `libwl-common`; both repos consume it (subtree/submodule) | **Recommended, Stage 2** — bounded churn |
| **C. Runtime feature dedup** | Keep both repos; delete duplicate *features*; cross-exec | **Recommended, Stage 1** — days, near-zero risk |
| **D. Status quo** | Nothing | **Rejected** — duplication is growing (ROADMAP Phase 15), and the palette has already drifted |

The options compose in order: C removes the remaining duplicate UI code, B
removes the maintenance cost, and A becomes a cheap final step if B has already
moved the shared seams.

---

## 6. Pros of consolidating

1. **Removes ~1,500–2,500 net LOC** of genuine duplication, including the
   unreachable bar power popover and the shelved in-bar launcher.
2. **One theme, one config, one look.** A palette change becomes one edit
   instead of a two-repo hand-sync that has already drifted (§2.3).
3. **One hardened spawn primitive.** After the lock-screen remediation, both
   projects need a `posix_spawn` + `pidfd` subprocess helper (I3). Today qypr has
   three `fork` call sites and waylaunch has its own wrapper.
4. **Process consolidation is available.** `qypr-bar` is already a resident
   layer-shell client owning an epoll loop, a seat, an icon resolver, and a
   toplevel backend — exactly the set `waylaunch --switcher` keeps resident for
   21.7 MB. Folding the switcher into the bar plausibly recovers ~20 MB and
   removes a lock file, a SIGUSR1/2 protocol, and the single-instance dance.
5. **Cross-features unlock.** waylaunch's content index could back a qypr-bar
   search popover (read-only, and never in `qypr-lock` — I4); qypr's
   MPRIS/battery/Wi-Fi/Bluetooth backends could become waylaunch
   `ResultProvider`s; the bar's launcher button could open the real Spotlight
   rather than its 228-line stand-in.
6. **One CI and one test convention.** 3.6k + 3.2k lines of tests currently sit
   in two different harnesses — and the I1–I4 tests would guard both.
7. **Protocol vendoring stops being a bug source** (§3.3).
8. **Naming honesty** — a consolidation is the natural moment to retire the
   `lockscreen` repository name.

## 7. Cons and risks

1. **Invariant erosion.** In one codebase, shared widget, input or spawn code
   can quietly re-open lock-screen surface. This is mitigated only if I1–I4 are
   enforced by tests in CI, not by code review.
2. **Attack- and dependency-surface growth (§4.2)** if binaries are unified;
   prevented by keeping separate binaries (I4).
3. **Convention churn destroys `git blame` (§4.3)** across both histories.
4. **Two active branches (§4.4)** — matters for Stage 4, much less for Stages
   1–2.
5. **Coupled release cadence.** Today a launcher regression cannot break the
   lock screen; after a shared `Painter`, it can. I1–I4 limit what a regression
   can *expose*, not whether it can *crash* the locker — and for a locker, a
   crash is a lockout. This is the strongest argument for keeping the
   lock-hosted part of the shared core small.
6. **Loss of independent bisectability** across the two histories.

---

## 8. Recommendation — staged consolidation

### Stage 1 — remove the remaining duplicate features (days, near-zero risk)

No shared code, no build changes, no architectural commitment.

- [x] ~~Route the bar's power entry to `waylaunch --power`~~ — **already done**
      in qypr `530998d` (2026-07-25).
- [ ] Delete the unreachable bar `PowerMenuPopover`
      (`PowerMenuIndicator.cpp:39-140`, 102 L) and the ignored `onPower`
      callback in `QuickSettingsPanel::buildTiles`. Keep the indicator itself —
      it is the Quick Settings trigger. **Keep `ui/PowerDialog`**: it is the lock
      screen's (§3.2).
- [ ] Point qypr's `LauncherIndicator` at `waylaunch` (through the I3 spawn
      path) and **delete `ui/statusbar/LauncherPopover`** (228 L).
- [ ] Copy qypr's full 407-line `wlr-layer-shell-unstable-v1.xml` over
      waylaunch's hand-reduced 151-line copy; re-run `wayland-scanner`; confirm
      no regression in the launcher, switcher, and power overlays.
- [ ] Reconcile the one drifted palette value (`warning`: `#f9e2af` vs
      `#fab387`) and record which is canonical.
- [ ] Document the `--switch` / `--switcher` / `--command-tab` aliases in the
      waylaunch README (`src/main.cpp:66` accepts all three; the live
      `binds.lua:126` uses `--switcher`, which the README still doesn't mention).

**Net:** ≈ 330 LOC deleted (`PowerMenuPopover` 102 + `LauncherPopover` 228), the
last duplicate UIs gone, one protocol bug class closed. Neither architecture is
touched.

### Stage 2 — extract a shared core, keep two repositories (weeks)

A `libwl-common` consumed by both as a git subtree (preferred over a submodule:
no detached-HEAD friction, vendored builds stay reproducible).

Extraction candidates, in dependency order:

1. `protocols/` — the union of all four XML files. **Do this one first**; it is
   pure data and validates the mechanism at zero risk.
2. `EventLoop` — take qypr's epoll reactor; it already has `addFd`/`addTimer`/
   `post`/`addPrepare` and is the more general of the two.
3. `Spawn` — the I3 primitive: `posix_spawn` with absolute paths,
   `POSIX_SPAWN_SETSIGDEF`, and `pidfd` reaping through `EventLoop`. Start from
   waylaunch's `search/subprocess.cpp`; it replaces qypr's three `fork` call
   sites.
4. `Painter` — qypr's cairo/pango helpers, extended with waylaunch's
   screencopy-backed blur.
5. `IconResolver` — qypr's (497 L, theme-inheritance aware, bounded LRU) is the
   more complete implementation.
6. `DesktopIndex` — qypr's, plus waylaunch's precomputed `search_key`
   optimisation (one `find()` per entry per keystroke instead of re-lowercasing
   four fields).
7. `ShmBuffer` + layer-shell surface setup.
8. `ToplevelBackend` — behind waylaunch's existing `IToplevelBackend` seam,
   which already exists for exactly this reason.

**Lock-hosted subset.** `qypr-lock` instantiates only `protocols/`, `EventLoop`,
`Spawn`, `Painter`, `IconResolver` (notification tiles) and `ShmBuffer`; it never
constructs `DesktopIndex` or `ToplevelBackend`. Changes to that subset should run
the I1–I4 tests.

Adopt qypr's `.clang-format` and naming **for the shared library only**, so the
convention churn is bounded to the extracted files and neither product's history
is rewritten wholesale. Both sides already write to interfaces
(`qypr/core/Interfaces.hpp`; waylaunch's `IToplevelBackend` /
`IPowerActionBackend` / `ResultProvider`), so the seams exist.

### Stage 3 — fold the switcher into `qypr-bar` (optional)

The largest single runtime win (~20 MB, one fewer resident process). `qypr-bar`
already owns every dependency the switcher needs. Requires Stage 2's
`ToplevelBackend` to be shared first.

### Stage 4 — monorepo (optional, no longer blocked)

One repository with a `common/` directory and separate binaries. Worth doing only
if Stage 2 has already moved most shared seams, so the remaining churn is small.
Preconditions:

- I1–I4 run as CI gates (the security review's §6 tests), including a CMake
  check that the `qypr-lock` target does not link `waylaunch_content` (I4).
- A decision on the convention split (§4.3) — or an explicit choice to keep both
  styles per directory.

### Explicitly not doing

- **Replacing the lock screen's `PowerDialog` with `waylaunch --power`** (§3.2).
- **Unifying the config format.** INI in qypr, TOML in waylaunch. Sharing a
  *palette file* (Stage 1) does not require sharing a *parser*.
- **Linking waylaunch's indexer into `qypr-lock`** (I4).

---

## 9. Open questions for the maintainer

1. ~~Does qypr's no-threads / no-spawn gate apply to the whole suite, or only to
   `qypr-lock`?~~ **Resolved in revision 2.** The gate is a footprint rule; the
   lock screen's security rests on I1–I4 (§4.1). Other binaries may use threads,
   and may spawn through the shared `Spawn` primitive.
2. **Which lock-screen interactions does I1 allow?** The security review
   suggests volume and brightness scroll plus media transport. That list is a
   product decision — and once widgets are shared, it becomes a contract every
   shared indicator has to honour.
3. **One repository or two?** Stages 2 and 4 work either way. Two repos preserve
   independent release cadence (§7.5); one repo removes the sync tax.
4. **Which name survives?** `qypr` is the broader brand (a desktop suite);
   `waylaunch` names one component. The `lockscreen` repo name should retire
   regardless.
5. **Is the ~20 MB from Stage 3 worth coupling the switcher's lifetime to the
   bar's?** A bar crash would currently take Alt+Tab with it.

---

## Appendix A — measurement method

Figures in §2–§3 were measured on 2026-08-25 against
`waylaunch@feat/spotlight-ui-refinement` and `qypr@qol`. Items marked
*re-verified 2026-09-15* were checked against `waylaunch@feat/dropdown-host`
(`485d9f0`) and `qypr@qol` (`27d7de1`).

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

# Revision 2 (2026-09-15): power routing and lock-only sources
git -C qypr log -S'waylaunch --power' --format='%h %ci %s' \
    -- src/ui/statusbar/QuickSettingsPanel.cpp        # → 530998d, 2026-07-25
grep -n 'id() == "power"' qypr/src/ui/statusbar/StatusBar.cpp
grep -n 'PowerDialog' qypr/CMakeLists.txt             # listed in QYPR_LOCK_ONLY_SOURCES
grep -n -E '^class PowerMenuPopover|^};' qypr/src/ui/indicators/PowerMenuIndicator.cpp
wc -l waylaunch/protocols/wlr-layer-shell-unstable-v1.xml
```
