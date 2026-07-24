#include "waylaunch/history.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>

int main() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("waylaunch-history-test-" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()) + ".tsv");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    waylaunch::HistoryStore store;
    store.configure(path.string(), 10, 365, 30.0);
    assert(store.load());
    constexpr std::time_t t0 = 1'800'000'000;
    store.record("terminal", "/usr/bin/alacritty", "Alacritty", t0);
    store.record("browser", "/usr/bin/firefox", "Firefox", t0 + 10);
    store.record("terminal", "/usr/bin/alacritty", "Alacritty", t0 + 20);

    const auto recent = store.recent(2);
    assert(recent.size() == 2);
    assert(recent[0].query == "terminal");
    assert(recent[0].uses == 2);
    assert(store.frecency("/usr/bin/alacritty", t0 + 20) >
           store.frecency("/usr/bin/firefox", t0 + 20));

    waylaunch::HistoryStore reloaded;
    reloaded.configure(path.string(), 10, 365, 30.0);
    assert(reloaded.load());
    assert(reloaded.recent(1)[0].query == "terminal");
    assert(reloaded.frecency("/usr/bin/alacritty", t0 + 20) > 0.0f);

    std::filesystem::remove(path, ec);
    return 0;
}
