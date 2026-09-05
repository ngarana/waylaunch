# AGENT.md — instructions for AI coding agents working in waylaunch

Read this file before any code task. It encodes repo conventions, quality
gates, and hard-won lessons. Violating it wastes a review cycle.

## 1. What this repo is

Waylaunch is a minimal, fast, keyboard-first Wayland-native launcher
(C++20, CMake). One binary, several modes: search launcher, app switcher
(`--switch`), power overlay (`--power`), dropdown terminal host
(`--dropdown`), plus the `waylaunchd` indexing daemon and `waylaunchctl`.

Layout: `src/` (implementation) mirrors `include/waylaunch/` (headers);
`tests/` holds assert-based unit tests; `config/` ships the default TOML;
`docs/` holds design/roadmap docs (`DESIGN.md` is the source of truth for
project scope); `doc/` holds the man page.

## 2. Essential commands

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
ctest --test-dir build -R <name> --output-on-failure   # single suite
./build/waylaunch --help
```

A configured `build/` directory must exist before any clang-tidy work — it
provides `compile_commands.json`.

## 3. Quality gates (non-negotiable)

CI (`.github/workflows/ci.yml`, `lint` job) enforces these. Local must match
CI exactly. Never declare work done with a red gate.

| Gate | Command |
|---|---|
| Format check | `clang-format --dry-run --Werror $(find src include tests -name '*.cpp' -o -name '*.h' \| grep -v "/build/")` |
| Format fix | `clang-format -i <files>` (same file set) |
| Tidy (single file, fast) | `clang-tidy -p build <file> --warnings-as-errors='*'` |
| Tidy (full, slow ~minutes) | `run-clang-tidy -p build -j 22 -warnings-as-errors='*'` |
| CMake targets | `cmake --build build --target format-check` |
| Pre-commit (all hooks) | `pre-commit run --all-files` (tidy hook is serial/slow; per-file runs are the norm) |

Configs: `.clang-format` (LLVM base, 4-space, `ColumnLimit: 100`,
`PointerAlignment/ReferenceAlignment: Left`, single-line guards allowed),
`.clang-tidy` (bugprone, clang-analyzer, modernize, performance,
portability, readability minus the disabled noisy checks — see file),
`.pre-commit-config.yaml` (whitespace/yaml/size/line-ending +
clang-format-check + clang-tidy-check on changed files).

Rules:

1. Run format + tidy on every file you touch, before finishing.
2. `clang-tidy` must pass with `--warnings-as-errors='*'` — the default
   invocation hides failures. Always append the flag when verifying.
3. If a check fires project-wide on idiomatic code (e.g. pointer-bool
   conversion, `#pragma once`), disable that check in `.clang-tidy` with a
   comment-worthy reason — do not sprinkle `NOLINT` or rewrite the codebase
   around a pedantic check.
4. `HeaderFilterRegex` is scoped to `waylaunch/(src|include|tests)` so system
   headers (glib/pango/harfbuzz) are never flagged. Keep it that way.
5. **Never run `clang-format` on `CMakeLists.txt` or any non-C/C++ file.**
   It mangles CMake syntax (splits `${VAR}`, reflows `#` comments) and breaks
   `cmake -S . -B build`. The pre-commit format hook is correctly scoped to
   `src|include|tests` + `*.cpp|*.h` — do not widen it.
6. `pre-commit install` must be active (hook lives at `.git/hooks/pre-commit`).
   Never commit with `--no-verify` unless the user explicitly approves, and
   say so in the final summary if you did.

## 4. Commit discipline — commit after every substantive change

Each completed unit of work (a feature slice, a bug fix, a gate rollout, a
doc update) ends in a commit on the current branch. Do not stockpile unrelated
changes in the working tree.

1. Inspect before staging: `git status --short`, `git diff`, and
   `git log --oneline -10` for message style. Stage only intended files;
   never commit secrets, tokens, or local-only paths.
2. Message style is conventional commits, lowercase scope:
   `fix(switcher): ...`, `feat(power): ...`, `chore(quality): ...`,
   `docs: ...`, `test: ...`, `config: ...`, `ci: ...`. One line subject,
   blank line, short body explaining why (not what — the diff shows what).
3. Let the pre-commit hooks run on `git commit`. If a hook fails, fix the
   finding and amend/retry — do not bypass. A hook failure is a finding, not
   an obstacle: e.g. a fresh tidy hit on a header means fixing the header.
4. Branch naming: `feat/<topic>`, `fix/<topic>`. The dropdown host lives on
   `feat/dropdown-host`; quality-gate work landed as `chore(quality)`.
5. Only commit, amend, push, or open PRs when the user asked. This section
   governs *how* to commit, not *whether* to.

## 5. Code conventions

- C++20, `-Wall -Wextra` (plus `-Wpedantic` on several targets). New code
  must be warning-free.
- New unit tests go in `tests/`, assert-based, registered in `CMakeLists.txt`
  with `-UNDEBUG` (without it, Release `NDEBUG` silently disables every
  `assert`). Mirror the `power_manager_test` pattern: pure logic behind an
  interface, `FakeBackend` records calls, no I/O in the unit under test.
- Every assert-based suite must carry `-UNDEBUG` — no exceptions. A local
  `build/` configured with `RelWithDebInfo`/`Release` once hid a real
  `Subprocess::run` stdin-direction bug behind a 21/21 "green" run while CI
  (default build type, asserts live) failed. Distrust any green run you did
  not verify: check `CMAKE_BUILD_TYPE` in `build/CMakeCache.txt`, and when
  in doubt rebuild the single test with `-UNDEBUG` and run it directly.
- New compositor coupling goes behind a seam interface (cf.
  `IToplevelBackend`, `IPlacementBackend`) so the core stays testable without
  a compositor. Keep every IPC/dispatcher string in the backend `.cpp`, none
  inline elsewhere — the protocol is stable but unversioned.
- Event sources are multiplexed through the owner's `poll()` (see
  `launcher_ui.cpp`). Never spawn a background thread for an event source.
- Thread slot/multi-instance identity through from day one (per-mode lock
  files, per-slot ids) — retrofitting singletons is invasive.
- Single-instance modes reuse `acquire_single_instance()` in `main.cpp`
  (flock + signal-to-incumbent), including the `SIG_IGN`-until-wired guard
  that fixed the rapid-second-invocation kill bug. New resident modes must
  inherit that guard for their toggle signal.
- Prefer `std::erase` / `std::erase_if` over hand-rolled
  `erase(remove(_if)(...), end())`. Note `std::ranges::remove_if` returns a
  subrange, not an iterator pair — feeding it to vector `erase` does not
  compile. Past `clang-tidy --fix` runs have introduced exactly this breakage;
  always rebuild after auto-fixes.

## 6. Gotchas ledger (append new entries here, don't re-learn them)

- Blocking `signalfd`/`timerfd` reads hang the daemon: create both with
  `SFD_NONBLOCK` / `TFD_NONBLOCK` and drain with `while (read(...) ==
  sizeof(...))`. A blocking drain loop parks in `signalfd_read_iter` forever
  after the last pending event (diagnosed live via `/proc/<pid>/wchan`).
- Never block shutdown on a child: SIGTERM, bounded `waitpid(WNOHANG)` grace
  period (~500ms), then SIGKILL. A stubborn child must not wedge exit.
- Hyprland Lua targeting: almost every `hl.dsp.window.*` takes
  `window=<selector|object>`; the error message lists only action keys.
  Exceptions: `bring_to_top()` takes no window arg (always acts on active —
  use `alter_zorder({mode="top", window})`), and `HL.Window` objects are
  data-only. Over the socket use `/dispatch <dispatcher-expr>` (bare
  dispatcher, no `hl.dispatch()` wrapper); `/eval` returns no values, so
  resolve addresses via `j/clients`. See `docs/DROPDOWN_IMPLEMENTATION.md` §2.
- Shell sessions reap background jobs: a daemon started with `&` under the
  tool session dies with its command. Detach smoke-test daemons with
  `setsid -f ... >/dev/null 2>&1` and clean up windows/pids afterwards.
- `pgrep` without `-f` matches only the 15-char comm name — use
  `ps aux | grep "[p]attern"` to check for running daemons without
  self-matching.

## 7. Docs map

- `docs/DESIGN.md` — project scope; advertised-but-unparsed config keys are a
  bug (§1.2). Wire parse + serialize in the same commit.
- `docs/DROPDOWN_TERMINAL.md` — why a dropdown host, not a terminal emulator.
- `docs/DROPDOWN_IMPLEMENTATION.md` — how: phases, module map, IPC
  transport, Spike 0 resolution. Implement phase by phase; each phase is
  independently shippable.
- `docs/POWER_MANAGER.md`, `docs/CONTENT_SEARCH.md` — subsystem references.
