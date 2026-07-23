# waylaunch

A minimal, fast, keyboard-first Wayland-native launcher. One keystroke opens a unified search bar that returns applications, files, calculator results, and custom commands together — grouped and ranked.

## Features

- **Unified Search** — no modes. Type once, get apps, files, folders, calculator results, and custom commands ranked together with a single Top Hit.
- **Frosted Glass** — client-side backdrop blur via `wlr-screencopy`. Captures the desktop, downsamples, and applies a separable box blur for a glassmorphism look. Works on any compositor.
- **Async File Search** — dedicated worker thread runs `fd` with prefix/substring ranking, recency bonuses, and path-depth penalties. Results stream into the UI without blocking.
- **Content Search** — index-backed full-text search *inside* your documents (Spotlight's "find in contents"), served by the `waylaunchd` daemon. An inverted index (SQLite FTS5, BM25) over text, code, PDF, **Office/ODF spreadsheets & slides** (`.docx`/`.xlsx`/`.pptx`/`.odt`/`.ods`/`.odp`), EPUB, and HTML; the launcher queries it read-only and shows a **CONTENTS** section with a highlighted snippet. Queries are O(index-lookup), not O(filesystem), and stay bounded even on ultra-common terms. Extracted text is stored zstd-compressed, extraction runs in a sandboxed subprocess, and the index self-heals on corruption or schema change. Kept fresh incrementally with inotify plus a periodic reconcile backstop for when watches are exhausted. Supports **`mdfind`-style filters** — `kind:pdf quarterly revenue`, `kind:spreadsheet`, `size:>1M`, `modified:<7d`. See [`docs/CONTENT_SEARCH.md`](docs/CONTENT_SEARCH.md).
- **App Switcher** — an Alt+Tab overlay grouping open windows by application (MRU-ordered) via `wlr-foreign-toplevel-management`. Launch it with `waylaunch --switch`: hold the modifier and tap Tab to cycle, release to confirm — or Enter/Space to confirm, ` / arrows to move, `q` to close an app, Esc to cancel. See [App switcher](#app-switcher-alttab) for the Hyprland binding.
- **Power Actions** — `waylaunch --power` opens a switcher-style HUD with Lock / Restart / Exit / Hibernate / Suspend / Shut Down. Destructive actions get a confirmation card; commands are configurable and run without a shell. See [Power actions](#power-actions).
- **Application Launcher** — scans `.desktop` files from XDG data directories, filters in-memory per keystroke.
- **Calculator** — built-in recursive-descent expression evaluator with trig, logs, and constants. A valid math expression becomes the Top Hit.
- **Custom Commands** — user-defined shell commands in the config file (Lock Screen, Sleep, etc.), matched by name.
- **Preview Pane** — two-column layout: results on the left, preview with details on the right. Right-click reveals files/apps in the file manager.
- **TOML Configuration** — modern, human-readable format with sane defaults.
- **Config Save** — snapshot current config (including merged defaults) with `waylaunch --save [path]`.
- **Theming** — customizable Catppuccin-inspired dark palette, fonts (family, size, weight, style), and layout.
- **Clipboard Integration** — file paths copied as `text/uri-list` on open; paste into search with `Ctrl+V`.
- **Keyboard-First** — navigate with arrows, vim/emacs bindings, tab-complete app names.

## Dependencies

### Runtime
- `wayland-client` — Wayland protocol library
- `wayland-cursor` — Cursor handling
- `libxkbcommon` — Keyboard handling
- `cairo` — 2D graphics
- `pango` + `pangocairo` — Text rendering
- `fontconfig` — Font discovery
- `gtk+-3.0` — Icon theme lookup (planned for removal)
- `gdk-pixbuf-2.0` — Image loading (planned for removal)
- `librsvg-2.0` — SVG icon rendering
- `sqlite` (FTS5) — content-search index
- `zstd` (`libzstd`) — compresses extracted document text in the index
- `file`/`libmagic` — MIME detection for the content indexer (optional; falls back to extensions)

### Optional (for file search)
- `fd` — Fast file finder (strongly recommended)

### Optional (for content extraction)
- `unzip` — Office/ODF/EPUB text (`.docx`/`.xlsx`/`.pptx`/`.odt`/`.ods`/`.odp`/`.epub` are ZIP containers; their XML parts are unzipped and stripped in-process — no heavy runtime needed)
- `poppler` (`pdftotext`) — PDF text
- `pandoc` — fallback for `.rtf` and any Office/ODF variant `unzip` can't read
- `odt2txt` — `.odt` fallback when pandoc is absent
- (plain text, code, Markdown, and HTML need no external tools)

### Build
- C++20 compiler (GCC 10+ or Clang 12+)
- CMake 3.20+
- pkg-config
- `wayland-scanner` + `wayland-protocols`

## Building

```bash
# Install dependencies (Arch Linux)
sudo pacman -S wayland wayland-protocols libxkbcommon cairo pango fontconfig \
               gtk3 gdk-pixbuf2 librsvg sqlite zstd file tomlplusplus

# Install optional file search + content extractors
sudo pacman -S fd unzip poppler pandoc odt2txt

# Clone and build
git clone https://github.com/yourusername/waylaunch.git
cd waylaunch
cmake -B build
cmake --build build

# Install
sudo cmake --install build
```

## Usage

```bash
# Run with default config (~/.config/waylaunch/config.toml)
waylaunch

# Run with custom config
waylaunch -c /path/to/config.toml

# Prefill the search query
waylaunch -q "fire"

# Save current config (merged defaults + overrides) to a file
waylaunch --save [path]

# Open the app switcher (normally bound to Alt+Tab — see below)
waylaunch --switch

# Open the power-actions overlay (see below)
waylaunch --power

# Debug output to stderr
waylaunch --debug
```

### App switcher (Alt+Tab)

`waylaunch --switch` opens a full-screen overlay that grabs the keyboard and
lists open windows grouped by application, most-recently-used first, with the
previous app preselected — so a quick Alt+Tab flips to the last window.

Bind it in Hyprland (`~/.config/hypr/hyprland.conf`):

```ini
# Alt+Tab → app switcher. Hold Alt and tap Tab to cycle; release Alt to confirm.
bind = ALT, Tab, exec, waylaunch --switch
# Optional: also open it going the other way (then Shift+Tab / ` cycles back).
bind = ALT SHIFT, Tab, exec, waylaunch --switch
```

How it works: the overlay maps as a `keyboard_interactivity: exclusive` layer
surface, so once it's up it owns the keyboard — subsequent Tab presses advance
the selection and the Alt release confirms and activates the chosen window.
Because Hyprland's keybind consumes each Tab before the surface sees it, a
repeated Alt+Tab re-runs `waylaunch --switch`, which signals the already-running
overlay to advance (single-instance via `$XDG_RUNTIME_DIR/waylaunch-switcher.lock`)
rather than stacking a new one. Inside the switcher: **Tab/→/`** next · **Shift+Tab/←**
previous · **release Alt / Enter / Space** confirm · **Esc** cancel · **q** close
the app · **h** minimize · **1–9** jump.

Requires a compositor that implements `wlr-foreign-toplevel-management` (Hyprland,
sway, and other wlroots-based compositors do).

**Cross-workspace:** activating a window is a standard `wlr-foreign-toplevel`
request; most compositors follow it to the window's workspace. If yours doesn't
(some custom/scripted setups), set `[app_switcher].activate_command` — it runs on
confirm with the window exported as `$WL_CLASS`/`$WL_TITLE`/`$WL_APP_ID`, e.g.
`hyprctl dispatch focuswindow class:"$WL_CLASS"`. See `config/waylaunch.toml`.

### Power actions

`waylaunch --power` opens a switcher-style frosted HUD with six power actions —
**Lock · Restart · Exit · Hibernate · Suspend · Shut Down** — as round icon
buttons with hand-drawn vector glyphs (crisp at any size, independent of the
icon theme) and the familiar selection pill. Keyboard-first, one-shot: pick an
action, it runs, the process exits.

Bind it in Hyprland (`~/.config/hypr/hyprland.conf`):

```ini
bind = SUPER SHIFT, Q, exec, waylaunch --power
```

Inside the overlay: **←/→/Tab/Shift+Tab** move · **Home/End** first/last ·
**1–6** jump · **Return/Space** select · **Esc** cancel. Destructive actions
(everything but Lock) open a glassmorphic confirmation card with a
**countdown**: a depleting ring around the action glyph and a counter in the
confirm button ("Shut Down · 42"); when it reaches zero the action runs on its
own (`countdown_seconds`, 0 disables). **←/→/Tab** move focus between Cancel
and the confirm button, **Return/Space** press the focused one, **Esc** always
goes back to the grid. The overlay tears down its surface *before* the command
runs, so it never lingers over a suspend or shutdown.

Everything is configurable under `[power]` (see `config/waylaunch.toml`):
`enabled_actions` filters and orders the cards (`[]` disables the overlay),
`[power.commands]` overrides any command (argv-split, executed without a
shell), `[power.confirm_text]` localizes the confirmation phrases, and
`confirm_destructive = false` skips the dialog globally. Defaults use
`systemctl` for the power verbs and `loginctl lock-session` for Lock; at
action time the power verbs are normalized to whichever binary owns them on
your init system (`systemctl` on systemd, `loginctl` on elogind), and "Exit"
prefers `wayland-logout`, falling back to terminating your login session. A
failed command is reported on stderr instead of exiting silently. A second
`waylaunch --power` while one is showing is a no-op (single-instance via
`$XDG_RUNTIME_DIR/waylaunch-power.lock`).

### Content search (the `waylaunchd` daemon)

Full-text search inside documents needs the indexing daemon running. It builds
and incrementally maintains the index; the launcher just queries it read-only,
so search stays instant and still works (filename-only) if the daemon is down.

```bash
# Start + enable at login (recommended)
systemctl --user enable --now waylaunchd

# ...or run it in the foreground
waylaunchd --config ~/.config/waylaunch/config.toml

# One-shot crawl (e.g. from cron) without staying resident
waylaunchd --once

# Inspect / control the running daemon
waylaunchctl status                 # index size, files, watches, reconcile state
waylaunchctl pause | resume         # suspend/resume indexing
waylaunchctl reindex                # rebuild the index from scratch
waylaunchctl reconcile              # re-scan roots to catch missed changes
waylaunchctl exclude ~/private      # stop indexing a path at runtime

# Query the index directly (read-only, like mdfind — works with the daemon down)
waylaunchctl search quarterly revenue
waylaunchctl search kind:pdf budget          # filter by kind + content
waylaunchctl search kind:spreadsheet         # browse a kind (no text term)
waylaunchctl search revenue modified:<7d     # content + recency
```

Queries accept `mdfind`-style predicates mixed with free text:
`kind:` (`pdf`/`image`/`audio`/`video`/`archive`/`doc`/`text`/`code`/`spreadsheet`/`presentation`),
`ext:xlsx`, `name:report`, `size:>1M` / `size:<500k`, and
`modified:<7d` / `modified:>2w` / `modified:today`.

Configure what gets indexed under `[content]` in the config (roots, excludes,
privacy paths, size caps, `prefix` vs `substring` matching, reconcile interval).
The daemon runs at idle CPU/IO priority with a bounded memory footprint and skips
a built-in privacy list (`~/.ssh`, `~/.gnupg`, …) by default.

## Configuration

Default config path: `~/.config/waylaunch/config.toml`. See `config/waylaunch.toml` for the full reference.

### Quick Start

```toml
[general]
debug = false

[appearance]
width         = 720
margin_top    = 150
corner_radius = 16
blur          = "auto"

[search]
placeholder    = "Type to search…"
applications   = true
files          = true
calculator     = true
commands       = true
file_roots     = ["~"]
max_file_results = 6

[theme.colors]
background = "#1e1e2e"
foreground = "#cdd6f4"
accent     = "#89b4fa"

[theme.result_font]
family = "Sans"
size   = 14.0
weight = "normal"      # normal | bold
style  = "normal"      # normal | italic | oblique

[[commands]]
name    = "Lock Screen"
command = "loginctl lock-session"
icon    = "system-lock-screen"
```

### Key sections

| Section | Description |
|---------|-------------|
| `[general]` | Runtime settings (debug mode, etc.) |
| `[appearance]` | Panel geometry and glassmorphism |
| `[theme]` | Colors and fonts (Catppuccin dark default) |
| `[search]` | Provider toggles, file search roots/excludes |
| `[content]` | Content-index daemon: roots, excludes, privacy paths, size caps, match mode, reconcile interval |
| `[app_switcher]` | Alt+Tab overlay config (modifier, icon size, grouping, quick actions) |
| `[power]` | Power overlay: action list/order, confirmation toggle, command + text overrides |
| `[[commands]]` | Custom shell commands shown as results |

### App switcher config

```toml
[app_switcher]
enabled        = true     # enable Alt+Tab overlay
modifier       = "Super"  # "Super" or "Alt"
icon_size      = 64       # icon size in px
card_size      = 104      # card size in px
corner_radius  = 20       # glass HUD corner radius
show_app_names = true     # show application title below HUD
group_by_app   = true     # true: one entry per app 
quick_actions  = true     # enable Q (quit app) and H (hide/minimize)

# Optional: compositor-specific fallback for compositors that don't follow the
# standard wlr-foreign-toplevel activate request to the window's workspace.
# activate_command = 'hyprctl dispatch focuswindow class:"$WL_CLASS"'
```

## Keybindings

| Key | Action |
|-----|--------|
| `Esc` | Dismiss |
| `Return` | Activate selected (launch app, open file, copy result, run command) |
| `Up` / `Down` | Navigate results |
| `Ctrl+J` / `Ctrl+K` | Vim-style navigation |
| `Ctrl+V` | Paste from clipboard into search field |
| `Tab` | Autocomplete app name |
| `PageUp` / `PageDown` | Jump by group |
| `Home` / `End` | First / last result |
| `Right-click` | Reveal in file manager |

## Architecture

```
src/
├── main.cpp                 # Entry point, signal handling
├── core/
│   └── wayland_core.cpp     # Wayland protocol, wlr-layer-shell, SHM buffers, screencopy
├── ui/
│   ├── renderer.cpp         # Cairo/Pango rendering, backdrop blur, icon cache
│   └── launcher_ui.cpp      # Main UI, event loop, unified search, worker thread
├── search/
│   └── subprocess.cpp       # Pipe-based subprocess management
├── modes/
│   ├── app_launcher.cpp     # .desktop file scanning and filtering
│   ├── calculator.cpp       # Recursive-descent expression parser
│   └── clipboard.cpp        # wl-copy / wl-paste integration
├── switcher/                 # app-switcher subsystem (Alt+Tab overlay)
│   ├── app_switcher_manager.cpp   # Open-window grouping, MRU ordering, icon resolution
│   ├── switcher_input_controller.cpp # Keyboard input for the switcher overlay
│   ├── switcher_renderer.cpp      # Cairo rendering of the switcher HUD
│   ├── switcher_state_machine.cpp # Interaction state machine
│   └── wlr_toplevel_backend.cpp   # wlr-foreign-toplevel-management protocol
├── power/                    # power-actions subsystem (Lock/Restart/Exit/…)
│   ├── power_manager.cpp          # Overlay lifecycle and action dispatch
│   ├── power_renderer.cpp         # Cairo rendering of frosted HUD and vector glyphs
│   ├── power_input_controller.cpp # Keyboard input for the power overlay
│   ├── power_state_machine.cpp    # Confirmation flow state machine
│   ├── power_action_backend.cpp   # Command normalization (systemd/elogind)
│   ├── power_glyphs.cpp           # Hand-drawn vector glyphs for power actions
│   └── confirm_dialog_renderer.cpp # Confirmation dialog with countdown
├── content/                 # content-search subsystem (lib + daemon + CLI)
│   ├── store.cpp            # SQLite FTS5 store: tokenizer, zstd docs, planner, metadata filters
│   ├── extractor.cpp        # MIME dispatch + per-format text extraction (sandboxed subprocesses)
│   ├── indexer.cpp          # crawl, change queue, content-hash skip, periodic reconcile
│   ├── fs_watcher.cpp       # inotify watcher (rename pairing, overflow, watch-limit fallback)
│   ├── control.cpp          # Unix control socket (status/pause/reindex/…)
│   ├── config.cpp           # [content] config parsing
│   ├── waylaunchd_main.cpp  # indexing daemon (≈ Spotlight's mds)
│   └── waylaunchctl_main.cpp# control + read-only search CLI (≈ mdutil/mdfind)
└── config/
    └── config.cpp           # TOML configuration parser
```

## Protocol Support

- **wlr-layer-shell** — Overlay panel with keyboard grab (wlroots compositors)
- **xdg-shell** — Fallback windowed mode (all compositors)
- **wlr-screencopy** — Desktop capture for client-side glassmorphism

## License

MIT
