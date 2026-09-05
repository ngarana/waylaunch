#include "waylaunch/config.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
    const auto stem = "waylaunch-config-test-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto input = std::filesystem::temp_directory_path() / (stem + ".toml");
    const auto output = std::filesystem::temp_directory_path() / (stem + "-saved.toml");

    {
        std::ofstream file(input);
        file << "[history]\n"
             << "enabled = false\n"
             << "max_entries = 17\n"
             << "max_age_days = 42\n"
             << "frecency_half_life_days = 12.5\n"
             << "\n[dropdown]\n"
             << "enabled = true\n"
             << "terminal = \"kitty\"\n"
             << "edge = \"bottom\"\n"
             << "width_percent = 90\n"
             << "height_percent = 50\n"
             << "hide_on_focus_loss = false\n"
             << "focus_grace_ms = 300\n"
             << "respawn = false\n"
             << "animation = \"fade\"\n"
             << "\n[[dropdown.slots]]\n"
             << "name = \"notes\"\n"
             << "command = \"kitty -e nvim\"\n"
             << "height_percent = 60\n";
    }

    waylaunch::Config config;
    assert(config.load(input.string()));
    assert(!config.get().history.enabled);
    assert(config.get().history.max_entries == 17);
    assert(config.get().history.max_age_days == 42);
    assert(config.get().history.frecency_half_life_days == 12.5);
    const auto& dropdown = config.get().dropdown;
    assert(dropdown.enabled);
    assert(dropdown.terminal == "kitty");
    assert(dropdown.edge == waylaunch::DropdownEdge::Bottom);
    assert(dropdown.width_percent == 90 && dropdown.height_percent == 50);
    assert(!dropdown.hide_on_focus_loss);
    assert(dropdown.focus_grace_ms == 300);
    assert(!dropdown.respawn);
    assert(dropdown.animation == "fade");
    assert(dropdown.slots.size() == 1);
    assert(dropdown.slots[0].name == "notes");
    assert(dropdown.slots[0].command == "kitty -e nvim");
    assert(dropdown.slots[0].width_percent == -1); // absent → inherit
    assert(dropdown.slots[0].height_percent == 60);
    assert(config.save(output.string()));

    waylaunch::Config reloaded;
    assert(reloaded.load(output.string()));
    assert(!reloaded.get().history.enabled);
    assert(reloaded.get().history.max_entries == 17);
    assert(reloaded.get().history.max_age_days == 42);
    assert(reloaded.get().history.frecency_half_life_days == 12.5);
    const auto& dd = reloaded.get().dropdown;
    assert(dd.enabled && dd.terminal == "kitty");
    assert(dd.edge == waylaunch::DropdownEdge::Bottom);
    assert(dd.width_percent == 90 && dd.height_percent == 50);
    assert(!dd.hide_on_focus_loss && dd.focus_grace_ms == 300);
    assert(!dd.respawn && dd.animation == "fade");
    assert(dd.slots.size() == 1 && dd.slots[0].name == "notes");
    assert(dd.slots[0].command == "kitty -e nvim");
    assert(dd.slots[0].width_percent == -1 && dd.slots[0].height_percent == 60);

    std::error_code ec;
    std::filesystem::remove(input, ec);
    std::filesystem::remove(output, ec);
    return 0;
}
