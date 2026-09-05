#include "waylaunch/dropdown/session_supervisor.h"

#include <cctype>
#include <cstdlib>

namespace waylaunch {
namespace {

// Whitespace split without shell processing: no globbing, no expansion, no
// quoting. Slot commands must use absolute paths ($HOME and ~ do not expand).
std::vector<std::string> split_words(const std::string& command) {
    std::vector<std::string> words;
    std::string word;
    for (char c : command) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!word.empty()) words.push_back(std::move(word));
            word.clear();
        } else {
            word.push_back(c);
        }
    }
    if (!word.empty()) words.push_back(std::move(word));
    return words;
}

std::vector<std::string> with_class_flags(const std::string& exec_term, const std::string& app_id,
                                          std::vector<std::string> rest) {
    // Match on the basename so absolute paths still map, but exec the
    // original term (it may not be on PATH).
    std::string base = exec_term.substr(exec_term.find_last_of('/') + 1);
    // Per-terminal class/app-id flags. Unknown terminals still launch; they
    // just cannot be matched by app-id until taught here.
    std::vector<std::string> argv;
    if (base == "kitty" || base == "alacritty") {
        argv = {exec_term, "--class", app_id};
    } else if (base == "foot") {
        argv = {exec_term, "--app-id", app_id};
    } else if (base == "wezterm") {
        argv = {exec_term, "--config", "window_id=" + app_id, "start"};
    } else {
        argv = {exec_term};
    }
    argv.insert(argv.end(), rest.begin(), rest.end());
    return argv;
}

} // namespace

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
    return with_class_flags(term, app_id, {});
}

std::vector<std::string> SessionSupervisor::build_slot_argv(const std::string& command,
                                                            const std::string& app_id) {
    std::vector<std::string> words = split_words(command);
    if (words.empty()) return build_argv("", app_id);
    std::string term = words.front();
    words.erase(words.begin());
    return with_class_flags(term, app_id, std::move(words));
}

} // namespace waylaunch
