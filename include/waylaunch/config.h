#pragma once

#include <string>
#include <vector>
#include <optional>
#include <toml++/toml.hpp>

namespace waylaunch {

struct ColorConfig {
    std::string background = "#1e1e2e";
    std::string background_alt = "#313244";
    std::string foreground = "#cdd6f4";
    std::string text_muted = "#6c7086";
    std::string accent = "#89b4fa";
    std::string accent_hover = "#b4d0fb";
    std::string error = "#f38ba8";
    std::string warning = "#fab387";
    std::string success = "#a6e3a1";
    std::string border = "#45475a";
    std::string selection = "#45475a";
};

struct ConfigFont {
    std::string family = "Sans";
    double size = 14.0;
    std::string weight = "normal";
    std::string style = "normal";
};

// Spotlight panel geometry + glassmorphism.
struct AppearanceConfig {
    int width          = 720;    // panel width in px
    int margin_top     = 150;    // distance from top of screen
    int corner_radius  = 16;
    int search_height  = 64;     // search field height
    int row_height     = 56;     // result row height
    int icon_size      = 34;
    int list_width     = 402;    // left result column width
    int max_per_group  = 6;      // max rows per category
    std::string blur   = "auto"; // "auto" | "on" | "off" (client-side frosted glass)
    double panel_opacity  = 0.58;// glass tint alpha when blurred
    double opaque_opacity = 0.96;// panel alpha when no blur is available
    double backdrop_tint  = 0.55;// darkening over the blurred backdrop
};

struct ThemeConfig {
    std::string name = "dark";
    std::string mode = "dark";
    std::string custom_path;
    ColorConfig colors;
    ConfigFont input_font;
    ConfigFont result_font;
    ConfigFont result_detail_font;
    int corner_radius = 12;
    double opacity = 0.95;
};

struct SearchPath {
    std::string path;
    std::string type;
    bool enabled = true;
};

struct SearchConfig {
    std::string placeholder = "Spotlight Search";
    std::vector<SearchPath> paths;   // .desktop scan dirs (+ non-desktop file roots)

    // Which result providers are active in the unified search.
    bool enable_applications = true;
    bool enable_files = true;
    bool enable_calculator = true;
    bool enable_commands = true;

    // File search (async `fd`) tuning.
    std::vector<std::string> file_roots;      // dirs to search; "~" expands to $HOME
    std::vector<std::string> file_excludes;   // fd --exclude patterns
    int file_min_query = 2;                   // min chars before file search runs
    int max_file_results = 6;                 // rows kept after ranking
};

struct Command {
    std::string name;
    std::string command;
    std::string icon;
    std::string shortcut;
    std::string category;
};

struct GeneralConfig {
    std::string app_name = "waylaunch";
    int version = 1;
    bool debug = false;
};

struct LauncherConfig {
    GeneralConfig general;
    AppearanceConfig appearance;
    ThemeConfig theme;
    SearchConfig search;
    std::vector<Command> commands;
};

class Config {
public:
    Config();
    ~Config();

    bool load(const std::string& path);
    bool save(const std::string& path) const;
    void set_defaults();

    const LauncherConfig& get() const { return config_; }
    LauncherConfig& get() { return config_; }

    static std::string default_config_path();
    static std::string config_dir();

private:
    LauncherConfig config_;
};

} // namespace waylaunch
