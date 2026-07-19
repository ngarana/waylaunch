# waylaunch

A minimal, fast, keyboard-first Wayland-native launcher inspired by macOS Spotlight. One keystroke opens a unified search bar that returns applications, files, calculator results, and custom commands together — grouped and ranked.

## Features

- **Unified Search** — no modes. Type once, get apps, files, folders, calculator results, and custom commands ranked together with a single Top Hit.
- **Frosted Glass** — client-side backdrop blur via `wlr-screencopy`. Captures the desktop, downsamples, and applies a separable box blur for a glassmorphism look. Works on any compositor.
- **Async File Search** — dedicated worker thread runs `fd` with prefix/substring ranking, recency bonuses, and path-depth penalties. Results stream into the UI without blocking.
- **Application Launcher** — scans `.desktop` files from XDG data directories, filters in-memory per keystroke.
- **Calculator** — built-in recursive-descent expression evaluator with trig, logs, and constants. A valid math expression becomes the Top Hit.
- **Custom Commands** — user-defined shell commands in the config file (Lock Screen, Sleep, etc.), matched by name.
- **Preview Pane** — two-column layout: results on the left, preview with details on the right. Right-click reveals files/apps in the file manager.
- **TOML Configuration** — modern, human-readable format with sane defaults.
- **Theming** — customizable Catppuccin-inspired dark palette, fonts, and layout.
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

### Optional (for file search)
- `fd` — Fast file finder (strongly recommended)

### Build
- C++20 compiler (GCC 10+ or Clang 12+)
- CMake 3.20+
- pkg-config
- `wayland-scanner` + `wayland-protocols`

## Building

```bash
# Install dependencies (Arch Linux)
sudo pacman -S wayland wayland-protocols libxkbcommon cairo pango fontconfig \
               gtk3 gdk-pixbuf2 librsvg

# Install optional file search
sudo pacman -S fd

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

# Debug output to stderr
waylaunch --debug
```

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
| `[[commands]]` | Custom shell commands shown as results |

## Keybindings

| Key | Action |
|-----|--------|
| `Esc` | Dismiss |
| `Return` | Activate selected (launch app, open file, copy result, run command) |
| `Up` / `Down` | Navigate results |
| `Ctrl+J` / `Ctrl+K` | Vim-style navigation |
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
│   ├── search_manager.cpp   # Search orchestration (unused currently)
│   └── subprocess.cpp       # Pipe-based subprocess management
├── modes/
│   ├── app_launcher.cpp     # .desktop file scanning and filtering
│   ├── calculator.cpp       # Recursive-descent expression parser
│   └── clipboard.cpp        # wl-copy integration
└── config/
    └── config.cpp           # TOML configuration parser
```

## Protocol Support

- **wlr-layer-shell** — Overlay panel with keyboard grab (wlroots compositors)
- **xdg-shell** — Fallback windowed mode (all compositors)
- **wlr-screencopy** — Desktop capture for client-side glassmorphism
- **wlr-foreign-toplevel** — Window management (unused currently)

## License

MIT
