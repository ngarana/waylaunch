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
                config_.theme.result_font.weight = get_str(*font, "weight", config_.theme.result_font.weight);
                config_.theme.result_font.style = get_str(*font, "style", config_.theme.result_font.style);
            }
            if (auto font = (*theme)["result_detail_font"].as_table()) {
                config_.theme.result_detail_font.family = get_str(*font, "family", config_.theme.result_detail_font.family);
                config_.theme.result_detail_font.size = get_double(*font, "size", config_.theme.result_detail_font.size);
                config_.theme.result_detail_font.weight = get_str(*font, "weight", config_.theme.result_detail_font.weight);
                config_.theme.result_detail_font.style = get_str(*font, "style", config_.theme.result_detail_font.style);
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

        if (auto switcher = tbl["app_switcher"].as_table()) {
            auto& sw = config_.app_switcher;
            sw.enabled = get_bool(*switcher, "enabled", sw.enabled);
            sw.modifier = get_str(*switcher, "modifier", sw.modifier);
            sw.icon_size = get_int(*switcher, "icon_size", sw.icon_size);
            sw.card_size = get_int(*switcher, "card_size", sw.card_size);
            sw.corner_radius = get_int(*switcher, "corner_radius", sw.corner_radius);
            sw.show_app_names = get_bool(*switcher, "show_app_names", sw.show_app_names);
            sw.group_by_app = get_bool(*switcher, "group_by_app", sw.group_by_app);
            sw.quick_actions = get_bool(*switcher, "quick_actions", sw.quick_actions);
            sw.activate_command = get_str(*switcher, "activate_command", sw.activate_command);
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

    file << "# waylaunch configuration — generated by --save\n\n";

    file << "[general]\n";
    file << "name = \"" << config_.general.app_name << "\"\n";
    file << "version = " << config_.general.version << "\n";
    file << "debug = " << (config_.general.debug ? "true" : "false") << "\n\n";

    file << "[appearance]\n";
    file << "width = " << config_.appearance.width << "\n";
    file << "margin_top = " << config_.appearance.margin_top << "\n";
    file << "corner_radius = " << config_.appearance.corner_radius << "\n";
    file << "search_height = " << config_.appearance.search_height << "\n";
    file << "row_height = " << config_.appearance.row_height << "\n";
    file << "icon_size = " << config_.appearance.icon_size << "\n";
    file << "list_width = " << config_.appearance.list_width << "\n";
    file << "max_per_group = " << config_.appearance.max_per_group << "\n";
    file << "blur = \"" << config_.appearance.blur << "\"\n";
    file << "panel_opacity = " << config_.appearance.panel_opacity << "\n";
    file << "opaque_opacity = " << config_.appearance.opaque_opacity << "\n";
    file << "backdrop_tint = " << config_.appearance.backdrop_tint << "\n\n";

    file << "[theme]\n";
    file << "name = \"" << config_.theme.name << "\"\n";
    file << "mode = \"" << config_.theme.mode << "\"\n";
    file << "custom_path = \"" << config_.theme.custom_path << "\"\n\n";

    auto write_color = [&](const std::string& k, const std::string& v) {
        file << k << " = \"" << v << "\"\n";
    };
    file << "[theme.colors]\n";
    write_color("background", config_.theme.colors.background);
    write_color("background_alt", config_.theme.colors.background_alt);
    write_color("foreground", config_.theme.colors.foreground);
    write_color("text_muted", config_.theme.colors.text_muted);
    write_color("accent", config_.theme.colors.accent);
    write_color("accent_hover", config_.theme.colors.accent_hover);
    write_color("error", config_.theme.colors.error);
    write_color("warning", config_.theme.colors.warning);
    write_color("success", config_.theme.colors.success);
    write_color("border", config_.theme.colors.border);
    write_color("selection", config_.theme.colors.selection);

    auto write_font = [&](const std::string& sec, const ConfigFont& f) {
        file << "\n[" << sec << "]\n";
        file << "family = \"" << f.family << "\"\n";
        file << "size = " << f.size << "\n";
        file << "weight = \"" << f.weight << "\"\n";
        file << "style = \"" << f.style << "\"\n";
    };
    write_font("theme.input_font", config_.theme.input_font);
    write_font("theme.result_font", config_.theme.result_font);
    write_font("theme.result_detail_font", config_.theme.result_detail_font);

    file << "\n[search]\n";
    file << "placeholder = \"" << config_.search.placeholder << "\"\n";
    file << "applications = " << (config_.search.enable_applications ? "true" : "false") << "\n";
    file << "files = " << (config_.search.enable_files ? "true" : "false") << "\n";
    file << "calculator = " << (config_.search.enable_calculator ? "true" : "false") << "\n";
    file << "commands = " << (config_.search.enable_commands ? "true" : "false") << "\n";
    file << "file_min_query = " << config_.search.file_min_query << "\n";
    file << "max_file_results = " << config_.search.max_file_results << "\n";

    if (!config_.search.file_roots.empty()) {
        file << "file_roots = [";
        for (size_t i = 0; i < config_.search.file_roots.size(); i++) {
            if (i > 0) file << ", ";
            file << "\"" << config_.search.file_roots[i] << "\"";
        }
        file << "]\n";
    }

    for (const auto& p : config_.search.paths) {
        file << "\n[[search.paths]]\n";
        file << "path = \"" << p.path << "\"\n";
        file << "type = \"" << p.type << "\"\n";
        file << "enabled = " << (p.enabled ? "true" : "false") << "\n";
    }

    for (const auto& cmd : config_.commands) {
        file << "\n[[commands]]\n";
        file << "name = \"" << cmd.name << "\"\n";
        file << "command = \"" << cmd.command << "\"\n";
        if (!cmd.icon.empty()) file << "icon = \"" << cmd.icon << "\"\n";
        if (!cmd.category.empty()) file << "category = \"" << cmd.category << "\"\n";
    }

    return true;
}

void Config::set_defaults() { config_ = LauncherConfig{}; }

} // namespace waylaunch
