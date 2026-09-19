#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "waylaunch/dropdown/hyprland_json.h"

namespace waylaunch {

// Exact-address workspace follow for the app switcher.
//
// Background: `WlrForeignToplevelBackend::activate()` sends the standard
// wlr-foreign-toplevel activate request, which stock compositors follow to the
// window's workspace — but some setups (e.g. Lua-scripted Hyprland) need an
// extra nudge. Users historically filled that gap with a title/class-based
// `activate_command`, which breaks on titles like "(17) WhatsApp - Helium":
// Hyprland's `title:`/`class:` selectors are regexes embedded in a Lua string
// embedded in shell, so `()[]..*+?` never match literally.
//
// This unit resolves the selected window to a Hyprland `address:0x...` via
// `j/clients` and focuses that. Addresses are hex — no regex metacharacters —
// and comparison here is exact `==` in C++, so no quoting layer can misfire.
// The pattern mirrors `src/dropdown/hyprland_backend.cpp` (same socket,
// same `address:` selector, same safe-no-op `ok` reply contract).

// Escape a value interpolated into a double-quoted Lua string.
std::string hypr_lua_escape(std::string_view value);

// Dispatch payloads for one address (no transport; pure, unit-tested).
// Lua-scripted Hyprland first, stock `hyprctl`-flavoured second.
std::string hypr_focus_payload_lua(const std::string& address);
std::string hypr_focus_payload_stock(const std::string& address);

// True only for plausible Hyprland addresses (hex, optional 0x prefix).
// Rejects anything that could break out of the Lua string.
bool hypr_address_is_safe(const std::string& address);

// Pick the Hyprland address for a wlr-foreign-toplevel window. Exact string
// equality throughout — never a regex match:
//   1. class == app_id AND title == title (first hit)
//   2. title-only exact match (covers XWayland class/app_id skew), first hit
//   3. class-only, but ONLY when it identifies a single client
// Otherwise nullopt (ambiguous — caller must not focus a guess).
std::optional<std::string> pick_hypr_address(const std::vector<HyprClient>& clients,
                                             const std::string& app_id, const std::string& title);

// Transport: one-shot AF_UNIX request to the Hyprland command socket.
// Returns nullopt when not on Hyprland or on any I/O failure.
std::string hypr_ipc_socket_path();
std::optional<std::string> hypr_ipc_request(const std::string& command);

// Focus one address. Tries the Lua dispatcher, then stock `focuswindow`.
// False on any miss — a miss is a safe no-op, never a fallback focus.
bool hypr_focus_address(const std::string& address);

// End-to-end: resolve (app_id, title) via `j/clients` and focus the address.
// Best-effort: false when Hyprland is absent, unreachable, or ambiguous.
bool hypr_focus_window(const std::string& app_id, const std::string& title);

} // namespace waylaunch
