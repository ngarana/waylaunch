#include "waylaunch/config.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
    const auto stem = "waylaunch-config-test-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto input = std::filesystem::temp_directory_path() / (stem + ".toml");
    const auto output = std::filesystem::temp_directory_path() / (stem + "-saved.toml");

    {
        std::ofstream file(input);
        file << "[history]\n"
             << "enabled = false\n"
             << "max_entries = 17\n"
             << "max_age_days = 42\n"
             << "frecency_half_life_days = 12.5\n";
    }

    waylaunch::Config config;
    assert(config.load(input.string()));
    assert(!config.get().history.enabled);
    assert(config.get().history.max_entries == 17);
    assert(config.get().history.max_age_days == 42);
    assert(config.get().history.frecency_half_life_days == 12.5);
    assert(config.save(output.string()));

    waylaunch::Config reloaded;
    assert(reloaded.load(output.string()));
    assert(!reloaded.get().history.enabled);
    assert(reloaded.get().history.max_entries == 17);
    assert(reloaded.get().history.max_age_days == 42);
    assert(reloaded.get().history.frecency_half_life_days == 12.5);

    std::error_code ec;
    std::filesystem::remove(input, ec);
    std::filesystem::remove(output, ec);
    return 0;
}
