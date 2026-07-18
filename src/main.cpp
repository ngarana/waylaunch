#include "waylaunch/config.h"
#include "waylaunch/launcher_ui.h"
#include <iostream>
#include <signal.h>
#include <unistd.h>
#include <string>

static volatile sig_atomic_t running = 1;

void signal_handler(int) { running = 0; }

void setup_signals() {
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

int main(int argc, char* argv[]) {
    setup_signals();

    waylaunch::Config config;
    std::string config_path;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "waylaunch - Wayland native launcher\nUsage: waylaunch [options]\n"
                      << "  -c, --config <path>  Config file path\n"
                      << "  -h, --help           Show help\n"
                      << "  -v, --version        Show version\n";
            return 0;
        } else if (arg == "--version" || arg == "-v") {
            std::cout << "waylaunch 0.1.0\n"; return 0;
        } else if (arg == "--debug") {
            config.get().general.debug = true;
        }
    }

    if (config_path.empty()) config_path = waylaunch::Config::default_config_path();

    if (!config.load(config_path)) {
        std::cerr << "Warning: Could not load config, using defaults.\n";
    }

    waylaunch::LauncherUI launcher;
    if (!launcher.init(config)) {
        std::cerr << "Error: Failed to initialize launcher\n";
        return 1;
    }

    launcher.run();
    return 0;
}
