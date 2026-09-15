#include "waylaunch/matugen_theme.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace waylaunch;

namespace {

int g_tmp_seq = 0;

std::string write_tmp(const std::string& body) {
    std::string path = (std::filesystem::temp_directory_path() /
                        ("matugen_theme_test_" + std::to_string(g_tmp_seq++) + ".json"))
                           .string();
    std::ofstream out(path, std::ios::trunc);
    assert(out.is_open());
    out << body;
    out.close();
    return path;
}

// Mirrors `matugen --json hex` (Material tokens; trimmed to the slots we map).
const char* kDump = R"({
  "colors": {
    "dark": {
      "background": "#1b1b1f",
      "on_surface": "#e3e2e6",
      "on_surface_variant": "#c4c6d0",
      "primary": "#adc6ff",
      "error": "#ffb4ab",
      "tertiary": "#e5b8e8",
      "secondary": "#bbc6e4",
      "outline_variant": "#44474f",
      "secondary_container": "#3b475f",
      "surface_container": "#252629"
    },
    "light": {
      "background": "#fefbff",
      "on_surface": "#1b1b1f",
      "primary": "#005ac1",
      "error": "#ba1a1a"
    }
  }
})";

void test_parse_dark() {
    std::map<std::string, std::string> scheme;
    std::map<std::string, std::string> direct;
    assert(MatugenTheme::parse_schemes(kDump, "dark", scheme, direct));
    assert(scheme["primary"] == "#adc6ff");
    assert(scheme["background"] == "#1b1b1f");
    assert(direct.empty());
    std::cout << "[PASS] parse dark scheme\n";
}

void test_parse_light_and_fallback() {
    std::map<std::string, std::string> scheme;
    std::map<std::string, std::string> direct;
    assert(MatugenTheme::parse_schemes(kDump, "light", scheme, direct));
    assert(scheme["primary"] == "#005ac1");
    assert(scheme["background"] == "#fefbff");
    // Unknown modes fall back to dark (theme.mode is free-form text).
    assert(MatugenTheme::parse_schemes(kDump, "banana", scheme, direct));
    assert(scheme["primary"] == "#adc6ff");
    std::cout << "[PASS] parse light scheme + dark fallback\n";
}

void test_parse_shapes() {
    std::map<std::string, std::string> scheme;
    std::map<std::string, std::string> direct;
    assert(!MatugenTheme::parse_schemes("{nope", "dark", scheme, direct));
    assert(!MatugenTheme::parse_schemes("{}", "dark", scheme, direct));
    assert(!MatugenTheme::parse_schemes("[1,2]", "dark", scheme, direct));
    // A template emitting our slot names directly parses as direct slots.
    assert(MatugenTheme::parse_schemes(R"({"background": "#aabbcc", "accent": "#112233"})", "dark",
                                       scheme, direct));
    assert(scheme.empty());
    assert(direct["background"] == "#aabbcc");
    std::cout << "[PASS] malformed + direct-slot shapes\n";
}

void test_apply_mapping() {
    std::map<std::string, std::string> scheme;
    std::map<std::string, std::string> direct;
    assert(MatugenTheme::parse_schemes(kDump, "dark", scheme, direct));
    ColorConfig out = MatugenTheme::apply_scheme(ColorConfig{}, scheme, direct);
    assert(out.background == "#1b1b1f");
    assert(out.background_alt == "#252629");
    assert(out.foreground == "#e3e2e6");
    assert(out.text_muted == "#c4c6d0");
    assert(out.accent == "#adc6ff");
    assert(out.accent_hover == "#bcd0ff"); // lighten(#adc6ff, 0.18)
    assert(out.error == "#ffb4ab");
    assert(out.warning == "#e5b8e8");
    assert(out.success == "#bbc6e4");
    assert(out.border == "#44474f");
    assert(out.selection == "#3b475f");
    std::cout << "[PASS] scheme token mapping\n";
}

void test_apply_partial_and_direct() {
    ColorConfig base;
    // Missing tokens keep the static base values.
    std::map<std::string, std::string> partial = {{"primary", "#010203"}};
    std::map<std::string, std::string> empty;
    ColorConfig out = MatugenTheme::apply_scheme(base, partial, empty);
    assert(out.accent == "#010203");
    assert(out.background == base.background);
    assert(out.border == base.border);
    // Direct slots win over scheme-mapped tokens.
    std::map<std::string, std::string> direct = {{"accent", "#111111"}};
    out = MatugenTheme::apply_scheme(base, partial, direct);
    assert(out.accent == "#111111");
    std::cout << "[PASS] partial fallback + direct precedence\n";
}

void test_custom_semantic_tokens() {
    // Dedicated warning/success tokens (matugen [config.custom_colors]) win
    // over the palette-hue fallbacks; without them tertiary/secondary apply.
    ColorConfig base;
    std::map<std::string, std::string> empty;
    std::map<std::string, std::string> fallback = {{"tertiary", "#e5b8e8"},
                                                   {"secondary", "#bbc6e4"}};
    ColorConfig out = MatugenTheme::apply_scheme(base, fallback, empty);
    assert(out.warning == "#e5b8e8");
    assert(out.success == "#bbc6e4");
    std::map<std::string, std::string> custom = {{"tertiary", "#e5b8e8"},
                                                 {"secondary", "#bbc6e4"},
                                                 {"warning", "#fab387"},
                                                 {"success", "#a6e3a1"}};
    out = MatugenTheme::apply_scheme(base, custom, empty);
    assert(out.warning == "#fab387");
    assert(out.success == "#a6e3a1");
    std::cout << "[PASS] custom semantic tokens\n";
}

void test_lighten() {
    assert(MatugenTheme::lighten_hex("#000000", 0.5) == "#808080");
    assert(MatugenTheme::lighten_hex("#ffffff", 0.9) == "#ffffff");
    assert(MatugenTheme::lighten_hex("#abc", 0.0) == "#aabbcc");
    assert(MatugenTheme::lighten_hex("#adc6ff", 0.18) == "#bcd0ff");
    assert(MatugenTheme::lighten_hex("#010203", 2.0) == "#ffffff"); // clamped
    assert(MatugenTheme::lighten_hex("not-a-color", 0.5) == "not-a-color");
    assert(MatugenTheme::lighten_hex("#12345", 0.5) == "#12345");
    std::cout << "[PASS] lighten_hex\n";
}

ThemeConfig matugen_config(const std::string& path, const std::string& mode) {
    ThemeConfig theme;
    theme.source = "matugen";
    theme.matugen_path = path;
    theme.mode = mode;
    return theme;
}

void test_resolve_file() {
    std::string path = write_tmp(kDump);
    MatugenTheme matugen;
    ColorConfig dark = matugen.resolve(matugen_config(path, "dark"));
    assert(dark.accent == "#adc6ff");
    assert(dark.background == "#1b1b1f");
    ColorConfig light = matugen.resolve(matugen_config(path, "light"));
    assert(light.accent == "#005ac1");
    // Missing and corrupt files fall back to the static colors, never empty.
    ColorConfig missing = matugen.resolve(matugen_config("/no/such/file.json", "dark"));
    assert(missing.accent == ColorConfig{}.accent);
    std::string bad = write_tmp("{corrupt");
    ColorConfig corrupt = matugen.resolve(matugen_config(bad, "dark"));
    assert(corrupt.accent == ColorConfig{}.accent);
    // Static source ignores the matugen file entirely.
    ThemeConfig plain;
    plain.matugen_path = path;
    assert(matugen.resolve(plain).accent == ColorConfig{}.accent);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(bad, ec);
    std::cout << "[PASS] resolve from file + fallbacks\n";
}

void test_live_reload() {
    std::string path = write_tmp(kDump);
    MatugenTheme matugen;
    ThemeConfig theme = matugen_config(path, "dark");
    assert(matugen.poll(theme));  // first poll always reports
    assert(!matugen.poll(theme)); // unchanged since
    // A wallpaper change rewrites the file: the next poll reports it and the
    // resolved colors follow without touching the static config.
    std::string changed = kDump;
    const std::string from = R"("primary": "#adc6ff")";
    assert(changed.find(from) != std::string::npos);
    changed.replace(changed.find(from), from.size(), R"("primary": "#000001")");
    std::ofstream out(path, std::ios::trunc);
    assert(out.is_open());
    out << changed;
    out.close();
    assert(matugen.poll(theme));
    assert(matugen.resolve(theme).accent == "#000001");
    assert(!matugen.poll(theme));
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::cout << "[PASS] live reload on rewrite\n";
}

} // namespace

int main() {
    test_parse_dark();
    test_parse_light_and_fallback();
    test_parse_shapes();
    test_apply_mapping();
    test_apply_partial_and_direct();
    test_custom_semantic_tokens();
    test_lighten();
    test_resolve_file();
    test_live_reload();
    std::cout << "matugen_theme_test: all passed\n";
    return 0;
}
