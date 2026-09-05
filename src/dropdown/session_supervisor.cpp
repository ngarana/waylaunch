#include "waylaunch/dropdown/session_supervisor.h"

#include <cstdlib>

namespace waylaunch {

void SessionSupervisor::note_spawned(pid_t pid, std::chrono::steady_clock::time_point now) {
    child_ = pid;
    spawned_at_ = now;
}

std::optional<std::chrono::milliseconds>
SessionSupervisor::note_exited(std::chrono::steady_clock::time_point now, bool respawn_enabled) {
    child_.reset();
    if (!respawn_enabled_ || !respawn_enabled) return std::nullopt;
    // A session that lived past a healthy lifetime was stable: restart fast.
    if (now - spawned_at_ >= std::chrono::seconds(kHealthyLifetimeSec)) {
        backoff_ms_ = kMinBackoffMs;
    }
    std::chrono::milliseconds delay(backoff_ms_);
    backoff_ms_ = std::min(backoff_ms_ * 2, kMaxBackoffMs);
    return delay;
}

std::vector<std::string> SessionSupervisor::build_argv(const std::string& terminal,
                                                       const std::string& app_id) {
    std::string term = terminal;
    if (term.empty()) {
        if (const char* env = std::getenv("TERMINAL"); env != nullptr && env[0] != '\0') {
            term = env;
        }
    }
    // Probe list when neither config nor $TERMINAL names one.
    if (term.empty()) term = "kitty";
    // Per-terminal class/app-id flags. Unknown terminals still launch; they
    // just cannot be matched by app-id until taught here.
    if (term == "kitty") return {"kitty", "--class", app_id};
    if (term == "foot") return {"foot", "--app-id", app_id};
    if (term == "alacritty") return {"alacritty", "--class", app_id};
    if (term == "wezterm") return {"wezterm", "--config", "window_id=" + app_id, "start"};
    return {term};
}

} // namespace waylaunch
