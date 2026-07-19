#include "waylaunch/config.h"
#include "waylaunch/launcher_ui.h"
#include <iostream>
#include <signal.h>
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
    std::string initial_query;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path = argv[++i];
        } else if ((arg == "--query" || arg == "-q") && i + 1 < argc) {
            initial_query = argv[++i];
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

    if (config_path.empty()) config_path = waylaunch::Config::default_config_path();

    if (!config.load(config_path)) {
        std::cerr << "Warning: Could not load config, using defaults.\n";
    }

    // Propagate debug flag to environment so worker threads see it too.
    if (config.get().general.debug) setenv("WAYLAUNCH_DEBUG", "1", 0);

    waylaunch::LauncherUI launcher;
    if (!initial_query.empty()) launcher.set_initial_query(initial_query);
    if (!launcher.init(config)) {
        std::cerr << "Error: Failed to initialize launcher\n";
        return 1;
    }

    launcher.run();
    return 0;
}
