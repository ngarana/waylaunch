# waylaunch

A minimal, fast, and customizable Wayland-native launcher with file search, application launching, and calculator functionality.

## Features

- **Wayland Native**: Built with raw Wayland protocols (no GTK/Qt dependencies)
- **Layer Shell Support**: Overlay launcher using wlr-layer-shell (wlroots compositors)
- **File Search**: Powered by `fd` and `fzf` for fast fuzzy file searching
- **Content Search**: Search file contents with `ripgrep`
- **Application Launcher**: Scans .desktop files for quick app launching
- **Calculator**: Built-in calculator with math functions
- **TOML Configuration**: Modern, human-readable config format
- **Theming**: Customizable colors and fonts
- **Keyboard-First**: Full keyboard navigation with vim/emacs bindings

## Dependencies

### Required
- `wayland-client` - Wayland protocol library
- `wayland-cursor` - Cursor handling
- `wayland-scanner` - Protocol code generation
- `wayland-protocols` - XDG shell and other protocols
- `libxkbcommon` - Keyboard handling
- `cairo` - 2D graphics
- `pango` + `pangocairo` - Text rendering
- `fontconfig` - Font discovery
- `freetype2` - Font rasterization

### Optional (for search functionality)
- `fd` - Fast file finder
- `fzf` - Fuzzy finder
- `ripgrep` - Content search

### Build Tools
- C++20 compiler (GCC 10+ or Clang 12+)
- CMake 3.20+
- pkg-config

## Building

```bash
# Install dependencies (Arch Linux)
sudo pacman -S wayland wayland-protocols libxkbcommon cairo pango fontconfig freetype2

# Install optional search tools
sudo pacman -S fd fzf ripgrep

# Clone and build
git clone https://github.com/yourusername/waylaunch.git
cd waylaunch
mkdir build && cd build
cmake ..
make

# Install
sudo make install
```

## Usage

```bash
# Run with default config
waylaunch

# Run with custom config
waylaunch --config ~/.config/waylaunch/config.toml

# Run in debug mode
waylaunch --debug
```

## Configuration

Configuration file is located at `~/.config/waylaunch/config.toml`.

### Example Config

```toml
[general]
name = "waylaunch"
version = 1
debug = false

[window]
width = 700
height = 500
position = "center"
margin = 100
corner_radius = 12
opacity = 0.95

[theme]
name = "dark"

[theme.colors]
background = "#1e1e2e"
foreground = "#cdd6f4"
accent = "#89b4fa"

[search]
match_mode = "fuzzy"
max_results = 20
debounce_ms = 150

[[search.paths]]
path = "/usr/share/applications"
type = "desktop"

[modes]
default = "applications"

[[modes.list]]
id = "applications"
name = "Apps"
enabled = true

[[modes.list]]
id = "files"
name = "Files"
enabled = true
```

## Keybindings

Default keybindings:

| Key | Action |
|-----|--------|
| `Return` | Launch selected item |
| `Escape` | Close launcher |
| `Up/Down` | Navigate results |
| `Tab` | Switch mode |
| `Ctrl+J/K` | Navigate (vim style) |
| `Ctrl+N/P` | Navigate (emacs style) |

## Search Modes

### Applications (`applications`)
Scans `.desktop` files from XDG data directories.

### Files (`files`)
Uses `fd` for file discovery and `fzf` for fuzzy filtering.

### Content (`contents`)
Searches file contents using `ripgrep` with JSON output.

### Calculator (`calculator`)
Evaluates mathematical expressions with support for:
- Basic operations: `+`, `-`, `*`, `/`, `^`
- Functions: `sin`, `cos`, `tan`, `sqrt`, `log`, `ln`, `abs`, etc.
- Constants: `pi`, `e`, `phi`
- Parentheses for grouping

## Architecture

```
src/
├── main.cpp                 # Entry point
├── core/
│   ├── wayland_core.cpp     # Wayland protocol handling
│   └── buffer.cpp           # Shared memory buffer management
├── ui/
│   ├── renderer.cpp         # Cairo/Pango rendering
│   └── launcher_ui.cpp      # Main UI logic
├── search/
│   ├── search_manager.cpp   # Search orchestration
│   ├── subprocess.cpp       # Process management
│   └── fuzzy_matcher.cpp    # Fuzzy matching algorithm
├── modes/
│   ├── app_launcher.cpp     # .desktop file parsing
│   ├── file_search.cpp      # File search utilities
│   ├── content_search.cpp   # Content search utilities
│   ├── calculator.cpp       # Expression parser
│   └── clipboard.cpp        # wl-copy integration
└── config/
    └── config.cpp           # TOML configuration parser
```

## Protocol Support

- **XDG Shell**: Standard windowed mode (all compositors)
- **wlr-layer-shell**: Overlay mode (wlroots compositors)
- **wlr-foreign-toplevel**: Window management (wlroots compositors)

## License

MIT License

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## Acknowledgments

- Inspired by [fzf](https://github.com/junegunn/fzf), [wofi](https://github.com/redelect/wofi), and [rofi](https://github.com/DaveDavenport/rofi)
- Uses [toml++](https://github.com/marzer/tomlplusplus) for TOML parsing
- Built with Cairo and Pango for rendering
