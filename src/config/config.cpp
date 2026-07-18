#include "waylaunch/config.h"
#include <fstream>
#include <sstream>
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

        if (auto win = tbl["window"].as_table()) {
            config_.window.width = get_int(*win, "width", config_.window.width);
            config_.window.height = get_int(*win, "height", config_.window.height);
            config_.window.position = get_str(*win, "position", config_.window.position);
            config_.window.margin = get_int(*win, "margin", config_.window.margin);
            config_.window.corner_radius = get_int(*win, "corner_radius", config_.window.corner_radius);
            config_.window.opacity = get_double(*win, "opacity", config_.window.opacity);
            config_.window.always_on_top = get_bool(*win, "always_on_top", config_.window.always_on_top);
            config_.window.decorations = get_bool(*win, "decorations", config_.window.decorations);
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
            config_.theme.corner_radius = get_int(*theme, "corner_radius", config_.window.corner_radius);
            config_.theme.opacity = get_double(*theme, "opacity", config_.window.opacity);
        }

        if (auto search = tbl["search"].as_table()) {
            config_.search.match_mode = get_str(*search, "match_mode", config_.search.match_mode);
            config_.search.case_sensitive = get_bool(*search, "case_sensitive", config_.search.case_sensitive);
            config_.search.max_results = get_int(*search, "max_results", config_.search.max_results);
            config_.search.show_icons = get_bool(*search, "show_icons", config_.search.show_icons);
            config_.search.debounce_ms = get_int(*search, "debounce_ms", config_.search.debounce_ms);
            config_.search.placeholder = get_str(*search, "placeholder", config_.search.placeholder);

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
            if (auto patterns = (*search)["file_patterns"].as_array()) {
                for (auto& entry : *patterns) {
                    if (auto tbl2 = entry.as_table()) {
                        FilePattern fp;
                        fp.extension = get_str(*tbl2, "extension");
                        fp.icon = get_str(*tbl2, "icon");
                        fp.label = get_str(*tbl2, "label");
                        config_.search.file_patterns.push_back(std::move(fp));
                    }
                }
            }
        }

        if (auto bindings = tbl["bindings"].as_table()) {
            if (auto keys = (*bindings)["keys"].as_array()) {
                for (auto& entry : *keys) {
                    if (auto tbl2 = entry.as_table()) {
                        KeyBinding kb;
                        kb.key = get_str(*tbl2, "key");
                        kb.action = get_str(*tbl2, "action");
                        kb.description = get_str(*tbl2, "description");
                        if (auto mods = (*tbl2)["modifiers"].as_array()) {
                            for (auto& m : *mods) {
                                if (auto s = m.value<std::string>()) {
                                    if (!kb.modifiers.empty()) kb.modifiers += "|";
                                    kb.modifiers += *s;
                                }
                            }
                        }
                        config_.bindings.push_back(std::move(kb));
                    }
                }
            }
        }

        if (auto modes = tbl["modes"].as_table()) {
            if (auto list = (*modes)["list"].as_array()) {
                for (auto& entry : *list) {
                    if (auto tbl2 = entry.as_table()) {
                        LauncherMode lm;
                        lm.id = get_str(*tbl2, "id");
                        lm.name = get_str(*tbl2, "name");
                        lm.icon = get_str(*tbl2, "icon");
                        lm.description = get_str(*tbl2, "description");
                        lm.enabled = get_bool(*tbl2, "enabled", true);
                        config_.modes.push_back(std::move(lm));
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

        if (auto platform = tbl["platform"].as_table()) {
            config_.platform.compositor = get_str(*platform, "compositor", config_.platform.compositor);
            config_.platform.use_layer_shell = get_bool(*platform, "use_layer_shell", config_.platform.use_layer_shell);
            config_.platform.show_on_all_workspaces = get_bool(*platform, "show_on_all_workspaces", config_.platform.show_on_all_workspaces);
            config_.platform.notifications = get_bool(*platform, "notifications", config_.platform.notifications);
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
