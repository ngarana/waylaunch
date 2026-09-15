#include "waylaunch/matugen_theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace waylaunch {
namespace {

// Amount primary is lifted toward white for the hover/derived accent slot.
constexpr double kHoverLighten = 0.18;
// Cap nesting so a hostile file cannot blow the stack in the recursive parser.
constexpr int kMaxJsonDepth = 32;

// Minimal JSON subset: objects with string keys; values are strings, nested
// objects, or anything else (arrays/numbers/bools/null are skipped). Enough
// for matugen --json hex dumps; anything richer is rejected by parse_root.
struct JsonValue {
    std::string str;
    std::map<std::string, JsonValue> obj;
    bool is_obj = false;
};

class JsonCursor {
  public:
    explicit JsonCursor(const std::string& text) : text_(text) {}

    bool parse_root(JsonValue& out) {
        skip_ws();
        if (!parse_value(out, 0)) return false;
        skip_ws();
        return pos_ == text_.size();
    }

  private:
    const std::string& text_;
    size_t pos_ = 0;

    void skip_ws() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool consume(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool parse_value(JsonValue& out, int depth) {
        skip_ws();
        if (pos_ >= text_.size()) return false;
        char c = text_[pos_];
        if (c == '{') return parse_object(out, depth);
        if (c == '"') {
            out = JsonValue{};
            return parse_string(out.str);
        }
        skip_other();
        out = JsonValue{};
        return true;
    }

    bool parse_object(JsonValue& out, int depth) {
        if (depth > kMaxJsonDepth) return false;
        if (!consume('{')) return false;
        out = JsonValue{};
        out.is_obj = true;
        skip_ws();
        if (consume('}')) return true;
        while (true) {
            std::string key;
            skip_ws();
            if (!parse_string(key)) return false;
            skip_ws();
            if (!consume(':')) return false;
            JsonValue val;
            if (!parse_value(val, depth + 1)) return false;
            out.obj.emplace(key, std::move(val));
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) return true;
            return false;
        }
    }

    bool parse_string(std::string& out) {
        if (!consume('"')) return false;
        out.clear();
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) return false;
            char e = text_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u':
                    if (pos_ + 4 > text_.size()) return false;
                    pos_ += 4;
                    out += '?'; // hex palettes never need real \u decoding
                    break;
                default: out += e; break;
            }
        }
        return false;
    }

    // Skip an array wholesale, or a scalar (number/bool/null) up to the next
    // structural character. String-aware so brackets inside strings don't end
    // the skip early.
    void skip_other() {
        if (pos_ < text_.size() && text_[pos_] == '[') {
            int depth = 0;
            bool in_str = false;
            while (pos_ < text_.size()) {
                char c = text_[pos_++];
                if (in_str) {
                    if (c == '\\') {
                        if (pos_ < text_.size()) ++pos_;
                    } else if (c == '"') {
                        in_str = false;
                    }
                } else if (c == '"') {
                    in_str = true;
                } else if (c == '[') {
                    ++depth;
                } else if (c == ']') {
                    if (--depth == 0) break;
                }
            }
            return;
        }
        bool in_str = false;
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (in_str) {
                ++pos_;
                if (c == '\\' && pos_ < text_.size()) {
                    ++pos_;
                } else if (c == '"') {
                    in_str = false;
                }
            } else if (c == '"') {
                in_str = true;
                ++pos_;
            } else if (c == ',' || c == '}' || c == ']') {
                break;
            } else {
                ++pos_;
            }
        }
    }
};

// First non-empty token from the scheme, or "" when none is present.
std::string pick_token(const std::map<std::string, std::string>& scheme,
                       std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        auto it = scheme.find(key);
        if (it != scheme.end() && !it->second.empty()) return it->second;
    }
    return "";
}

void use_token(std::string& slot, const std::string& token) {
    if (!token.empty()) slot = token;
}

std::string expand_home(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') return path;
    return std::string(home) + path.substr(1);
}

} // namespace

std::string MatugenTheme::default_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0') return std::string(xdg) + "/matugen/colors.json";
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0')
        return std::string(home) + "/.config/matugen/colors.json";
    return "";
}

std::string MatugenTheme::resolve_path(const ThemeConfig& theme) {
    if (!theme.matugen_path.empty()) return expand_home(theme.matugen_path);
    return default_path();
}

bool MatugenTheme::parse_schemes(const std::string& json, const std::string& mode,
                                 std::map<std::string, std::string>& scheme,
                                 std::map<std::string, std::string>& direct) {
    scheme.clear();
    direct.clear();
    JsonCursor cur(json);
    JsonValue root;
    if (!cur.parse_root(root) || !root.is_obj) return false;
    for (const auto& [key, val] : root.obj) {
        if (!val.is_obj && !key.empty()) direct[key] = val.str;
    }
    auto it = root.obj.find("colors");
    if (it == root.obj.end() || !it->second.is_obj) return !direct.empty();
    const auto& colors = it->second.obj;
    const std::string want = (mode == "light") ? "light" : "dark";
    const JsonValue* sel = nullptr;
    auto found = colors.find(want);
    if (found != colors.end() && found->second.is_obj) {
        sel = &found->second;
    } else {
        auto dark = colors.find("dark");
        if (dark != colors.end() && dark->second.is_obj) sel = &dark->second;
    }
    if (sel == nullptr) return !direct.empty();
    for (const auto& [key, val] : sel->obj) {
        if (!val.is_obj) scheme[key] = val.str;
    }
    return true;
}

ColorConfig MatugenTheme::apply_scheme(const ColorConfig& base,
                                       const std::map<std::string, std::string>& scheme,
                                       const std::map<std::string, std::string>& direct) {
    ColorConfig out = base;
    use_token(out.background, pick_token(scheme, {"background"}));
    use_token(out.background_alt, pick_token(scheme, {"surface_container", "surface_container_high",
                                                      "surface_variant"}));
    use_token(out.foreground, pick_token(scheme, {"on_surface"}));
    use_token(out.text_muted, pick_token(scheme, {"on_surface_variant"}));
    const std::string primary = pick_token(scheme, {"primary"});
    use_token(out.accent, primary);
    if (!primary.empty()) out.accent_hover = lighten_hex(primary, kHoverLighten);
    use_token(out.error, pick_token(scheme, {"error"}));
    // Semantic slots prefer dedicated tokens (matugen [config.custom_colors],
    // harmonized with the wallpaper by default) and only fall back to palette
    // hues: Material You defines no warning/success roles.
    use_token(out.warning, pick_token(scheme, {"warning", "tertiary"}));
    use_token(out.success, pick_token(scheme, {"success", "secondary"}));
    use_token(out.border, pick_token(scheme, {"outline_variant", "outline"}));
    use_token(out.selection, pick_token(scheme, {"secondary_container", "primary_container"}));
    // A template emitting our slot names directly states intent; it wins.
    use_token(out.background, pick_token(direct, {"background"}));
    use_token(out.background_alt, pick_token(direct, {"background_alt"}));
    use_token(out.foreground, pick_token(direct, {"foreground"}));
    use_token(out.text_muted, pick_token(direct, {"text_muted"}));
    use_token(out.accent, pick_token(direct, {"accent"}));
    use_token(out.accent_hover, pick_token(direct, {"accent_hover"}));
    use_token(out.error, pick_token(direct, {"error"}));
    use_token(out.warning, pick_token(direct, {"warning"}));
    use_token(out.success, pick_token(direct, {"success"}));
    use_token(out.border, pick_token(direct, {"border"}));
    use_token(out.selection, pick_token(direct, {"selection"}));
    return out;
}

std::string MatugenTheme::lighten_hex(const std::string& hex, double amount) {
    amount = std::clamp(amount, 0.0, 1.0);
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    if (h.size() == 3) {
        std::string full;
        full.reserve(6);
        for (char c : h) {
            full.push_back(c);
            full.push_back(c);
        }
        h = full;
    }
    if (h.size() != 6) return hex;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int rgb[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        size_t o = static_cast<size_t>(i) * 2;
        int hi = nibble(h[o]);
        int lo = nibble(h[o + 1]);
        if (hi < 0 || lo < 0) return hex;
        int v = (hi * 16) + lo;
        int lifted = v + static_cast<int>(std::lround((255 - v) * amount));
        rgb[i] = (lifted > 255) ? 255 : lifted;
    }
    char buf[8] = {};
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
    return {buf};
}

ColorConfig MatugenTheme::resolve(const ThemeConfig& theme) {
    if (theme.source != "matugen") return theme.colors;
    const std::string path = resolve_path(theme);
    if (path.empty()) return theme.colors;
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) return theme.colors; // missing (yet): static fallback, retry next call
    // The cache key includes the scheme mode: dark and light resolve from the
    // same file but select different token sets.
    const std::string mode = (theme.mode == "light") ? "light" : "dark";
    if (path == cached_path_ && mode == cached_mode_ && cached_mtime_.has_value() &&
        *cached_mtime_ == mtime) {
        return apply_scheme(theme.colors, cached_scheme_, cached_direct_);
    }
    std::ifstream file(path);
    if (!file.is_open()) return theme.colors;
    std::ostringstream text;
    text << file.rdbuf();
    std::map<std::string, std::string> scheme;
    std::map<std::string, std::string> direct;
    if (!parse_schemes(text.str(), theme.mode, scheme, direct)) {
        return theme.colors; // corrupt or mid-write: keep statics, retry next call
    }
    cached_path_ = path;
    cached_mode_ = mode;
    cached_mtime_ = mtime;
    cached_scheme_ = std::move(scheme);
    cached_direct_ = std::move(direct);
    return apply_scheme(theme.colors, cached_scheme_, cached_direct_);
}

bool MatugenTheme::poll(const ThemeConfig& theme) {
    ColorConfig now = resolve(theme);
    if (!have_last_ || !(now == last_colors_)) {
        last_colors_ = now;
        have_last_ = true;
        return true;
    }
    return false;
}

} // namespace waylaunch
