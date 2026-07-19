#include "waylaunch/content/config.h"

#include <toml++/toml.hpp>

#include <cstdlib>
#include <unistd.h>

namespace waylaunch::content {

namespace {

std::string home_dir() {
    const char* h = getenv("HOME");
    return h ? std::string(h) : std::string();
}

std::string default_config_path() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) return std::string(xdg) + "/waylaunch/config.toml";
    std::string h = home_dir();
    if (!h.empty()) return h + "/.config/waylaunch/config.toml";
    return "./config.toml";
}

// Read an array-of-strings TOML node into a vector (const or mutable node-view).
template <typename NodeView>
std::vector<std::string> str_array(NodeView node) {
    std::vector<std::string> out;
    if (auto arr = node.as_array())
        for (auto& e : *arr)
            if (auto s = e.template value<std::string>()) out.push_back(*s);
    return out;
}

std::vector<std::string> expand_all(std::vector<std::string> v) {
    for (auto& s : v) s = expand_tilde(s);
    return v;
}

} // namespace

std::string expand_tilde(const std::string& p) {
    if (p.empty() || p[0] != '~') return p;
    std::string h = home_dir();
    if (h.empty()) return p;
    if (p.size() == 1) return h;
    if (p[1] == '/') return h + p.substr(1);
    return p;  // ~user unsupported
}

std::string ContentConfig::runtime_dir() {
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0]) return std::string(xdg) + "/waylaunch";
    // Fallback: a private dir under /tmp keyed by uid.
    return "/tmp/waylaunch-" + std::to_string(getuid());
}

std::string ContentConfig::socket_path() { return runtime_dir() + "/waylaunchd.sock"; }

std::string ContentConfig::db_path() {
    const char* xdg = getenv("XDG_DATA_HOME");
    std::string base;
    if (xdg && xdg[0]) base = std::string(xdg) + "/waylaunch";
    else {
        std::string h = home_dir();
        base = h.empty() ? "./.local/share/waylaunch" : h + "/.local/share/waylaunch";
    }
    return base + "/index.db";
}

ExtractOptions ContentConfig::extract_options() const {
    ExtractOptions o;
    o.max_text_bytes = max_text_mb * 1024 * 1024;
    o.max_read_bytes = max_file_mb * 1024 * 1024;
    o.nice = worker_nice;
    return o;
}

ContentConfig load_content_config(const std::string& config_path) {
    ContentConfig c;
    std::string path = config_path.empty() ? default_config_path() : config_path;

    // Sensible privacy defaults regardless of config (NFR7 / §8).
    std::string h = home_dir();
    if (!h.empty()) {
        c.exclude_paths = {h + "/.ssh", h + "/.gnupg", h + "/.mozilla",
                           h + "/.local/share/keyrings", h + "/.password-store",
                           h + "/.config/waylaunch"};
    }

    toml::table tbl;
    try {
        tbl = toml::parse_file(path);
    } catch (...) {
        // No/invalid config: fall back to defaults + $HOME root.
        if (c.roots.empty() && !h.empty()) c.roots = {h};
        return c;
    }

    // [search] fallbacks for roots/excludes.
    std::vector<std::string> search_roots, search_excludes;
    if (auto s = tbl["search"].as_table()) {
        search_roots = expand_all(str_array((*s)["file_roots"]));
        search_excludes = str_array((*s)["file_excludes"]);
    }

    if (auto ct = tbl["content"].as_table()) {
        const toml::table& t = *ct;
        if (auto v = t["enable"].value<bool>()) c.enable = *v;
        c.roots = expand_all(str_array(t["roots"]));
        c.excludes = str_array(t["excludes"]);
        // Merge user-specified privacy paths onto the built-in defaults.
        for (auto& p : expand_all(str_array(t["exclude_paths"])))
            c.exclude_paths.push_back(p);
        if (auto v = t["max_file_mb"].value<int64_t>()) c.max_file_mb = static_cast<size_t>(*v);
        if (auto v = t["max_text_mb"].value<int64_t>()) c.max_text_mb = static_cast<size_t>(*v);
        if (auto v = t["max_index_mb"].value<int64_t>()) c.max_index_mb = static_cast<size_t>(*v);
        if (auto v = t["min_query"].value<int64_t>()) c.min_query = static_cast<int>(*v);
        if (auto v = t["max_results"].value<int64_t>()) c.max_results = static_cast<int>(*v);
        if (auto v = t["worker_nice"].value<int64_t>()) c.worker_nice = static_cast<int>(*v);
        if (auto v = t["throttle_on_battery"].value<bool>()) c.throttle_on_battery = *v;
        if (auto v = t["match"].value<std::string>())
            c.match = (*v == "substring") ? MatchMode::Substring : MatchMode::Prefix;
        auto ex = str_array(t["extractors"]);
        if (!ex.empty()) c.extractors = ex;
    }

    // Roots: [content].roots → [search].file_roots → $HOME.
    if (c.roots.empty()) c.roots = search_roots;
    if (c.roots.empty() && !h.empty()) c.roots = {h};

    // Excludes: union of [content].excludes and [search].file_excludes, plus a
    // built-in noise list so an unconfigured user still skips junk.
    if (c.excludes.empty()) c.excludes = search_excludes;
    if (c.excludes.empty())
        c.excludes = {".git", "node_modules", ".cache", "target", ".venv",
                      "__pycache__", ".cargo", ".rustup", "go/pkg",
                      ".local/share/Trash", ".npm", "build"};
    return c;
}

} // namespace waylaunch::content
