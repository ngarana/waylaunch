#include "waylaunch/config.h"
#include "waylaunch/launcher_ui.h"
#include <iostream>
#include <string>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {

// Single-instance guard: the launcher/switcher is a keyboard-grabbing overlay,
// so a second invocation of the same bind must not stack a new window. We hold
// an flock on a per-mode lock file for our lifetime. If another instance already
// holds it, we send it `existing_signal` and step aside — SIGTERM for the search
// launcher (the Super+D bind toggles it closed), SIGUSR1 for the switcher (a
// repeated Alt+Tab advances the running switcher). Returns the held fd (keep it
// open), -1 to "exit now", or -2 if the lock is unavailable (proceed anyway).
int acquire_single_instance(const char* lock_name, int existing_signal) {
    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    std::string path = (rt && rt[0] ? std::string(rt) : "/tmp") + "/" + lock_name;
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -2;
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
        if (::ftruncate(fd, 0) == 0) {
            std::string pid = std::to_string(::getpid()) + "\n";
            ssize_t w = ::write(fd, pid.data(), pid.size());
            (void)w;
        }
        return fd;   // we own it; keep the fd open for our lifetime
    }
    // Another instance holds the lock → signal it and step aside.
    char buf[32] = {0};
    ::lseek(fd, 0, SEEK_SET);
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        long other = std::atol(buf);
        if (other > 1) ::kill(static_cast<pid_t>(other), existing_signal);
    }
    ::close(fd);
    return -1;
}

} // namespace

// SIGINT/SIGTERM are handled inside LauncherUI::run() (wired into its poll loop),
// so the keyboard-grabbing overlay exits cleanly on a kill signal.
int main(int argc, char* argv[]) {
    waylaunch::Config config;
    std::string config_path;
    std::string initial_query;
    bool switcher_mode = false;
    bool switcher_reverse = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path = argv[++i];
        } else if ((arg == "--query" || arg == "-q") && i + 1 < argc) {
            initial_query = argv[++i];
        } else if (arg == "--switch" || arg == "--switcher" || arg == "--command-tab") {
            switcher_mode = true;
        } else if (arg == "--reverse") {
            switcher_reverse = true;   // preselect the far end (Alt+Shift+Tab)
        } else if (arg == "--save") {
            config_path = (i + 1 < argc) ? argv[++i] : waylaunch::Config::default_config_path();
            if (!config.load(config_path)) {
                std::cerr << "Warning: Could not load config from " << config_path << ", using defaults.\n";
            }
            if (config.save(config_path)) {
                std::cout << "Saved config to " << config_path << "\n";
            } else {
                std::cerr << "Error: Failed to save config to " << config_path << "\n";
                return 1;
            }
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "waylaunch - Wayland native launcher\nUsage: waylaunch [options]\n"
                      << "  -c, --config <path>  Config file path\n"
                      << "  -q, --query <text>   Prefill the search query\n"
                      << "  --switch             Open the app switcher (bind to Alt+Tab)\n"
                      << "  --save [path]        Serialize current config to a file\n"
                      << "  --debug              Enable debug output\n"
                      << "  -h, --help           Show this help\n"
                      << "  -v, --version        Show version\n";
            return 0;
        } else if (arg == "--version" || arg == "-v") {
            std::cout << "waylaunch 0.1.0\n"; return 0;
        } else if (arg == "--debug") {
            config.get().general.debug = true;
        }
    }

    // Single-instance: the search launcher toggles (Super+D opens/closes); the
    // switcher advances a running instance (a repeated Alt+Tab), keeping them on
    // separate locks so opening one never disturbs the other.
    int lock_fd = switcher_mode
        ? acquire_single_instance("waylaunch-switcher.lock", SIGUSR1)
        : acquire_single_instance("waylaunch-launcher.lock", SIGTERM);
    if (lock_fd == -1) return 0;
    (void)lock_fd;   // held open for our lifetime; the OS releases it on exit

    if (config_path.empty()) config_path = waylaunch::Config::default_config_path();

    if (!config.load(config_path)) {
        std::cerr << "Warning: Could not load config, using defaults.\n";
    }

    // Propagate debug flag to environment so worker threads see it too.
    if (config.get().general.debug) setenv("WAYLAUNCH_DEBUG", "1", 0);

    waylaunch::LauncherUI launcher;
    if (!initial_query.empty()) launcher.set_initial_query(initial_query);
    launcher.set_config_path(config_path);
    launcher.set_switcher_mode(switcher_mode);
    launcher.set_switcher_reverse(switcher_reverse);
    if (!launcher.init(config)) {
        std::cerr << "Error: Failed to initialize launcher\n";
        return 1;
    }

    launcher.run();
    return 0;
}
