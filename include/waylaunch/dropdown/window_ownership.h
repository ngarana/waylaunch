#pragma once

#include "waylaunch/dropdown/hyprland_json.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace waylaunch {

// Which window belongs to a dropdown slot. Matching on the app-id alone is
// ambiguous — anyone can start a terminal with the slot's class — and acting
// on the wrong one means placing, hiding, or resizing a window the user owns.
// The supervised pid disambiguates, so this is the rule everything that
// touches a slot window goes through.
//
// Pure and ancestry-injectable, so it unit-tests without a compositor or
// /proc; the backend passes is_descendant_process.
enum class Ownership {
    Exact = 0,      // the supervised process itself
    Descendant = 1, // spawned by it — terminals re-exec and multiplex
    ClassOnly = 2,  // right class, unrelated process
};

// (client_pid, owner_pid) -> is the first a descendant of (or equal to) the second.
using AncestryFn = std::function<bool(int, int)>;

Ownership classify_ownership(int client_pid, int owner_pid, const AncestryFn& ancestry);

// Best-owned client whose class is `app_id`, or npos. Ties keep the earliest
// (compositor order). `owner_pid <= 0` means "unknown", which degrades to the
// first class match — the behaviour a restarted daemon needs to adopt the
// window it left behind.
size_t select_owned(const std::vector<HyprClient>& clients, const std::string& app_id,
                    int owner_pid, const AncestryFn& ancestry);

// Indices of every client belonging to the slot, in compositor order. With a
// known owner, class-only matches are intruders and are excluded; without one
// every class match is the best guess available.
std::vector<size_t> filter_owned(const std::vector<HyprClient>& clients, const std::string& app_id,
                                 int owner_pid, const AncestryFn& ancestry);

} // namespace waylaunch
