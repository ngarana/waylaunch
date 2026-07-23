#include "waylaunch/power/power_action_backend.h"
#include "waylaunch/config.h"
#include "waylaunch/subprocess.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace waylaunch {

namespace {

bool power_dbg() { static bool v = std::getenv("WAYLAUNCH_DEBUG") != nullptr; return v; }

constexpr const char* kSubtext =
    "Open apps will be closed. Unsaved work will be lost.";

// The six built-in actions (§4.1). `confirm_text` here is the localizable
// phrase; the full headline is composed below so a [power.confirm_text]
// override slots into the same sentence.
struct ActionDefault {
    const char* id;
    const char* name;
    const char* command;
    const char* icon;
    const char* phrase;        // "" = non-destructive, no dialog
    bool session_ending;       // subtext + red confirm button
};

// The four system power verbs live on `systemctl` under systemd but on
// `loginctl` under elogind — systemd's loginctl does NOT know reboot/suspend/
// poweroff/hibernate. Default to systemctl (the common case); execute()
// normalizes to whichever binary actually exists at action time.
constexpr ActionDefault kDefaults[] = {
    {"lock",      "Lock",      "loginctl lock-session", "system-lock-screen",       "",                              false},
    {"restart",   "Restart",   "systemctl reboot",      "system-reboot",            "restart your computer",         true},
    {"exit",      "Exit",      "wayland-logout",        "system-log-out",           "log out now",                   true},
    {"hibernate", "Hibernate", "systemctl hibernate",   "system-suspend-hibernate", "hibernate your computer",       false},
    {"suspend",   "Suspend",   "systemctl suspend",     "system-suspend",           "put your computer to sleep",    false},
    {"shutdown",  "Shut Down", "systemctl poweroff",    "system-shutdown",          "shut down your computer now",   true},
};

bool is_power_verb(const std::string& v) {
    return v == "poweroff" || v == "reboot" || v == "suspend" || v == "hibernate";
}

std::string compose_headline(const std::string& phrase) {
    return "Are you sure you want to " + phrase + "?";
}

} // namespace

std::vector<std::string> PowerActionBackend::split_argv(const std::string& command) {
    std::vector<std::string> argv;
    std::string cur;
    bool in_word = false;
    char quote = 0;
    for (char c : command) {
        if (quote) {
            if (c == quote) quote = 0;
            else cur += c;
        } else if (c == '\'' || c == '"') {
            quote = c;
            in_word = true;
        } else if (c == ' ' || c == '\t') {
            if (in_word) { argv.push_back(cur); cur.clear(); in_word = false; }
        } else {
            cur += c;
            in_word = true;
        }
    }
    if (in_word) argv.push_back(cur);
    return argv;
}

PowerActionBackend::PowerActionBackend(const PowerConfig& cfg) {
    // enabled_actions drives both filtering and display order (§4.6).
    for (const auto& id : cfg.enabled_actions) {
        const auto* def = std::find_if(std::begin(kDefaults), std::end(kDefaults),
                                       [&](const ActionDefault& d) { return id == d.id; });
        if (def == std::end(kDefaults)) continue;   // unknown id: ignore

        PowerAction a;
        a.id = def->id;
        a.name = def->name;
        a.icon_name = def->icon;
        a.destructive = def->phrase[0] != '\0';
        if (def->session_ending) a.subtext = kSubtext;

        auto cmd = cfg.commands.find(a.id);
        a.argv = split_argv(cmd != cfg.commands.end() ? cmd->second : def->command);

        if (a.destructive) {
            auto phrase = cfg.confirm_text.find(a.id);
            a.confirm_text = compose_headline(
                phrase != cfg.confirm_text.end() ? phrase->second : def->phrase);
        }
        actions_.push_back(std::move(a));
    }
}

std::vector<std::string> PowerActionBackend::resolve_argv(const PowerAction& action) {
    std::vector<std::string> argv = action.argv;

    // "Exit" prefers wayland-logout; the fallback is chosen at action time, not
    // hardcoded into config (§4.1): terminate this session, else this user.
    if (action.id == "exit" && !argv.empty() && !Subprocess::command_exists(argv[0])) {
        const char* sid = std::getenv("XDG_SESSION_ID");
        if (sid && sid[0])
            argv = {"loginctl", "terminate-session", sid};
        else
            argv = {"loginctl", "terminate-user", std::to_string(::getuid())};
    }

    // Same action-time choice for the power verbs: pick the binary that can
    // actually perform them here — systemctl on systemd, loginctl on elogind.
    // Also rescues configs carrying the other init system's spelling.
    if (argv.size() >= 2 && is_power_verb(argv[1])) {
        if (argv[0] == "loginctl" && Subprocess::command_exists("systemctl"))
            argv[0] = "systemctl";
        else if (argv[0] == "systemctl" && !Subprocess::command_exists("systemctl") &&
                 Subprocess::command_exists("loginctl"))
            argv[0] = "loginctl";
    }
    return argv;
}

int PowerActionBackend::execute(const PowerAction& action) {
    std::vector<std::string> argv = resolve_argv(action);
    if (argv.empty()) return -1;
    if (power_dbg()) {
        fprintf(stderr, "[power] execute id=%s argv0=%s\n",
                action.id.c_str(), argv[0].c_str());
    }

    ProcessResult res = Subprocess::run(argv);
    if (res.exit_code != 0) {
        // Failure reporting is not command logging (§7): say which action broke
        // and why, so a misconfigured command isn't a silent no-op.
        std::string err = res.stderr;
        while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
        fprintf(stderr, "waylaunch: power action '%s' failed (exit %d)%s%s\n",
                action.id.c_str(), res.exit_code,
                err.empty() ? "" : ": ", err.c_str());
    }
    return res.exit_code;
}

} // namespace waylaunch
