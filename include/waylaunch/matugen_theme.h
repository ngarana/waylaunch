#pragma once

#include "waylaunch/config.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace waylaunch {

// Matugen (Material You) theming: reads the `matugen --json hex` output file
// (usually written via a matugen template to
// `~/.config/matugen/colors.json`) and maps its Material tokens onto our 11
// [theme.colors] slots. Missing tokens or a missing file keep the static
// [theme.colors] values, so a partial/stale file never breaks the UI.
//
// Accepted file shapes:
//   1. Native matugen dump: {"colors": {"dark": {...}, "light": {...}}}
//      (theme.mode picks the scheme; falls back to "dark").
//   2. A matugen template emitting our slot names directly, e.g.
//      {"background": "{{colors.surface.dark.hex}}", "accent": ...}.
//      Direct slots win over scheme-mapped tokens.
//
// Token mapping (scheme -> slot):
//   background <- background
//   background_alt <- surface_container | surface_container_high | surface_variant
//   foreground <- on_surface
//   text_muted <- on_surface_variant
//   accent <- primary
//   accent_hover <- lighten(primary) (deterministic in both modes)
//   error <- error
//   warning <- warning | tertiary
//   success <- success | secondary
//   border <- outline_variant | outline
//   selection <- secondary_container | primary_container
//
// Semantic slots: Material You defines error but no warning/success roles,
// so those fall back to palette hues (tertiary/secondary) unless the scheme
// carries dedicated tokens. Define them in matugen so they stay semantic AND
// harmonize with the wallpaper (blend is on by default):
//   [config.custom_colors]
//   warning = "#fab387"
//   success = "#a6e3a1"
// A custom `error` overrides the scheme error the same way.
//
// Live reload: MatugenTheme caches the file by mtime and re-reads only when
// it moves. Call resolve()/poll() from the owner's existing poll loop (never
// from a background thread); a wallpaper change then repaints within one
// poll quantum with no restart.
class MatugenTheme {
  public:
    MatugenTheme() = default;

    // Default matugen colors path: $XDG_CONFIG_HOME/matugen/colors.json,
    // else ~/.config/matugen/colors.json.
    static std::string default_path();

    // Effective colors for a ThemeConfig: static [theme.colors], overlaid
    // with the matugen scheme when theme.source == "matugen". Re-reads the
    // file only when its mtime moved (or when the path changed).
    ColorConfig resolve(const ThemeConfig& theme);

    // True when resolve() output changed since the previous poll() call
    // (also true on the first call). Drives needs_redraw_.
    bool poll(const ThemeConfig& theme);

    // --- Pure helpers (unit-tested) ---
    // Parse matugen --json hex output, extracting the string tokens of the
    // requested scheme ("dark"/"light", falls back to "dark"). False when
    // the scheme is absent. Also collects top-level string pairs into
    // `direct` (shape 2 above); empty when none.
    static bool parse_schemes(const std::string& json, const std::string& mode,
                              std::map<std::string, std::string>& scheme,
                              std::map<std::string, std::string>& direct);
    // Overlay scheme (+ direct slots) onto base. Missing keys keep base.
    static ColorConfig apply_scheme(const ColorConfig& base,
                                    const std::map<std::string, std::string>& scheme,
                                    const std::map<std::string, std::string>& direct);
    // Lighten a #rrggbb (or #rgb, with/without '#') hex toward white by
    // amount in [0,1]. Returns the input unchanged when unparseable.
    static std::string lighten_hex(const std::string& hex, double amount);

  private:
    static std::string resolve_path(const ThemeConfig& theme);

    std::string cached_path_;
    std::string cached_mode_;
    std::optional<std::filesystem::file_time_type> cached_mtime_;
    std::map<std::string, std::string> cached_scheme_;
    std::map<std::string, std::string> cached_direct_;
    ColorConfig last_colors_;
    bool have_last_ = false;
};

} // namespace waylaunch
