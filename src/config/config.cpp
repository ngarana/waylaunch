#include "waylaunch/config.h"
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <iostream>

namespace waylaunch {

Config::Config() = default;
Config::~Config() = default;

std::string Config::config_dir() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] != '\0') return std::string(xdg) + "/waylaunch";
    const char* home = getenv("HOME");
    if (home) return std::string(home) + "/.config/waylaunch";
    return "./.config/waylaunch";
}

std::string Config::default_config_path() { return config_dir() + "/config.toml"; }

static std::string get_str(const toml::table& t, const std::string& key, const std::string& def = "") {
    auto node = t[key];
    if (node) {
        if (auto s = node.value<std::string>()) return *s;
    }
    return def;
}

static int get_int(const toml::table& t, const std::string& key, int def = 0) {
    auto node = t[key];
    if (node) {
        if (auto s = node.value<int>()) return *s;
    }
    return def;
}

static double get_double(const toml::table& t, const std::string& key, double def = 0.0) {
    auto node = t[key];
    if (node) {
        if (auto s = node.value<double>()) return *s;
    }
    return def;
}

static bool get_bool(const toml::table& t, const std::string& key, bool def = false) {
    auto node = t[key];
    if (node) {
        if (auto s = node.value<bool>()) return *s;
    }
    return def;
}

bool Config::load(const std::string& path) {
    try {
        auto tbl = toml::parse_file(path);

        if (auto general = tbl["general"].as_table()) {
            config_.general.app_name = get_str(*general, "name", config_.general.app_name);
            config_.general.version = get_int(*general, "version", config_.general.version);
            config_.general.debug = get_bool(*general, "debug", config_.general.debug);
        }

        if (auto theme = tbl["theme"].as_table()) {
            config_.theme.name = get_str(*theme, "name", config_.theme.name);
            config_.theme.mode = get_str(*theme, "mode", config_.theme.mode);
            config_.theme.custom_path = get_str(*theme, "custom_path", config_.theme.custom_path);

            if (auto colors = (*theme)["colors"].as_table()) {
                config_.theme.colors.background = get_str(*colors, "background", config_.theme.colors.background);
                config_.theme.colors.background_alt = get_str(*colors, "background_alt", config_.theme.colors.background_alt);
                config_.theme.colors.foreground = get_str(*colors, "foreground", config_.theme.colors.foreground);
                config_.theme.colors.text_muted = get_str(*colors, "text_muted", config_.theme.colors.text_muted);
                config_.theme.colors.accent = get_str(*colors, "accent", config_.theme.colors.accent);
                config_.theme.colors.accent_hover = get_str(*colors, "accent_hover", config_.theme.colors.accent_hover);
                config_.theme.colors.error = get_str(*colors, "error", config_.theme.colors.error);
                config_.theme.colors.warning = get_str(*colors, "warning", config_.theme.colors.warning);
                config_.theme.colors.success = get_str(*colors, "success", config_.theme.colors.success);
                config_.theme.colors.border = get_str(*colors, "border", config_.theme.colors.border);
                config_.theme.colors.selection = get_str(*colors, "selection", config_.theme.colors.selection);
            }
            if (auto font = (*theme)["input_font"].as_table()) {
                config_.theme.input_font.family = get_str(*font, "family", config_.theme.input_font.family);
                config_.theme.input_font.size = get_double(*font, "size", config_.theme.input_font.size);
            }
            if (auto font = (*theme)["result_font"].as_table()) {
                config_.theme.result_font.family = get_str(*font, "family", config_.theme.result_font.family);
                config_.theme.result_font.size = get_double(*font, "size", config_.theme.result_font.size);
            }
            config_.theme.corner_radius = get_int(*theme, "corner_radius", config_.theme.corner_radius);
            config_.theme.opacity = get_double(*theme, "opacity", config_.theme.opacity);
        }

        if (auto ap = tbl["appearance"].as_table()) {
            auto& a = config_.appearance;
            a.width = get_int(*ap, "width", a.width);
            a.margin_top = get_int(*ap, "margin_top", a.margin_top);
            a.corner_radius = get_int(*ap, "corner_radius", a.corner_radius);
            a.search_height = get_int(*ap, "search_height", a.search_height);
            a.row_height = get_int(*ap, "row_height", a.row_height);
            a.icon_size = get_int(*ap, "icon_size", a.icon_size);
            a.list_width = get_int(*ap, "list_width", a.list_width);
            a.max_per_group = get_int(*ap, "max_per_group", a.max_per_group);
            a.blur = get_str(*ap, "blur", a.blur);
            a.panel_opacity = get_double(*ap, "panel_opacity", a.panel_opacity);
            a.opaque_opacity = get_double(*ap, "opaque_opacity", a.opaque_opacity);
            a.backdrop_tint = get_double(*ap, "backdrop_tint", a.backdrop_tint);
        }

        if (auto search = tbl["search"].as_table()) {
            config_.search.placeholder = get_str(*search, "placeholder", config_.search.placeholder);

            config_.search.enable_applications = get_bool(*search, "applications", config_.search.enable_applications);
            config_.search.enable_files = get_bool(*search, "files", config_.search.enable_files);
            config_.search.enable_calculator = get_bool(*search, "calculator", config_.search.enable_calculator);
            config_.search.enable_commands = get_bool(*search, "commands", config_.search.enable_commands);
            config_.search.file_min_query = get_int(*search, "file_min_query", config_.search.file_min_query);
            config_.search.max_file_results = get_int(*search, "max_file_results", config_.search.max_file_results);

            if (auto roots = (*search)["file_roots"].as_array()) {
                config_.search.file_roots.clear();
                for (auto& e : *roots)
                    if (auto s = e.value<std::string>()) config_.search.file_roots.push_back(*s);
            }
            if (auto ex = (*search)["file_excludes"].as_array()) {
                config_.search.file_excludes.clear();
                for (auto& e : *ex)
                    if (auto s = e.value<std::string>()) config_.search.file_excludes.push_back(*s);
            }

            if (auto paths = (*search)["paths"].as_array()) {
                for (auto& entry : *paths) {
                    if (auto tbl2 = entry.as_table()) {
                        SearchPath sp;
                        sp.path = get_str(*tbl2, "path");
                        sp.type = get_str(*tbl2, "type");
                        sp.enabled = get_bool(*tbl2, "enabled", true);
                        config_.search.paths.push_back(std::move(sp));
                    }
                }
            }
        }

        if (auto commands = tbl["commands"].as_array()) {
            for (auto& entry : *commands) {
                if (auto tbl2 = entry.as_table()) {
                    Command cmd;
                    cmd.name = get_str(*tbl2, "name");
                    cmd.command = get_str(*tbl2, "command");
                    cmd.icon = get_str(*tbl2, "icon");
                    cmd.shortcut = get_str(*tbl2, "shortcut");
                    cmd.category = get_str(*tbl2, "category");
                    config_.commands.push_back(std::move(cmd));
                }
            }
        }

        return true;
    } catch (const toml::parse_error& err) {
        std::cerr << "Config parse error: " << err.what() << "\n";
        set_defaults();
        return false;
    }
}

bool Config::save(const std::string& path) const {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << "# waylaunch configuration\n\n";
    file << "[general]\nname = \"" << config_.general.app_name << "\"\n\n";
    return true;
}

void Config::set_defaults() { config_ = LauncherConfig{}; }

} // namespace waylaunch
