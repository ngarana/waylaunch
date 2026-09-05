#pragma once

#include <ctime>
#include <string>

namespace waylaunch {

// Path/format helpers shared between the UI thread and the search-worker
// providers (file/content), so ranking + display formatting live in one place.

std::string home_dir();                               // $HOME (or "." if unset)
std::string abbreviate_home(const std::string& path); // leading $HOME -> "~"
std::string icon_for_file(const std::string& path);   // extension -> freedesktop icon name
int path_depth(const std::string& path);              // number of '/' separators
float recency_bonus(std::time_t mtime);               // more-recent mtime -> larger score bonus

// Content snippets carry matched runs wrapped in these sentinel bytes (the hl
// markers passed to Store::search). They're control chars the extractor's
// sanitizer strips from body text, so they never collide with real content.
inline constexpr char kHlOpen = '\x02';
inline constexpr char kHlClose = '\x03';

} // namespace waylaunch
