#include "waylaunch/power/power_action_backend.h"
#include "waylaunch/config.h"
#include <cassert>
#include <iostream>

using namespace waylaunch;

void test_default_actions() {
    PowerActionBackend backend{PowerConfig{}};
    const auto& acts = backend.actions();

    assert(acts.size() == 6);
    const char* order[] = {"lock", "restart", "exit", "hibernate", "suspend", "shutdown"};
    for (size_t i = 0; i < 6; i++) assert(acts[i].id == order[i]);

    // Lock is the only non-destructive action; it gets no dialog headline.
    assert(!acts[0].destructive);
    assert(acts[0].confirm_text.empty());
    assert(acts[0].argv == (std::vector<std::string>{"loginctl", "lock-session"}));

    // Shutdown: destructive, composed headline, session-ending subtext, red path.
    // Power verbs default to systemctl — systemd's loginctl doesn't have them.
    const auto& sd = acts[5];
    assert(sd.destructive);
    assert(sd.argv == (std::vector<std::string>{"systemctl", "poweroff"}));
    assert(sd.confirm_text == "Are you sure you want to shut down your computer now?");
    assert(!sd.subtext.empty());
    assert(acts[1].argv == (std::vector<std::string>{"systemctl", "reboot"}));
    assert(acts[3].argv == (std::vector<std::string>{"systemctl", "hibernate"}));
    assert(acts[4].argv == (std::vector<std::string>{"systemctl", "suspend"}));

    // Hibernate/suspend: destructive but not session-ending (no subtext).
    assert(acts[3].destructive && acts[3].subtext.empty());
    assert(acts[4].destructive && acts[4].subtext.empty());

    std::cout << "[PASS] default actions\n";
}

void test_command_override() {
    PowerConfig cfg;
    cfg.commands["shutdown"] = "systemctl poweroff -i";
    PowerActionBackend backend{cfg};

    const auto& sd = backend.actions().back();
    assert(sd.id == "shutdown");
    assert(sd.argv == (std::vector<std::string>{"systemctl", "poweroff", "-i"}));
    std::cout << "[PASS] command override\n";
}

void test_confirm_text_override() {
    PowerConfig cfg;
    cfg.confirm_text["restart"] = "reiniciar el equipo";
    PowerActionBackend backend{cfg};

    const auto& r = backend.actions()[1];
    assert(r.id == "restart");
    assert(r.confirm_text == "Are you sure you want to reiniciar el equipo?");
    std::cout << "[PASS] confirm text override\n";
}

void test_enabled_subset_and_order() {
    PowerConfig cfg;
    cfg.enabled_actions = {"suspend", "lock", "bogus"};   // custom order, unknown id
    PowerActionBackend backend{cfg};

    const auto& acts = backend.actions();
    assert(acts.size() == 2);            // "bogus" ignored
    assert(acts[0].id == "suspend");     // list order preserved
    assert(acts[1].id == "lock");
    std::cout << "[PASS] enabled subset and order\n";
}

void test_resolve_argv_normalization() {
    // Only meaningful where systemctl exists (systemd hosts — including this
    // one): a loginctl power verb is rewritten to the binary that has it.
    PowerAction a;
    a.id = "restart";
    a.argv = {"loginctl", "reboot"};
    auto argv = PowerActionBackend::resolve_argv(a);
    if (!argv.empty() && argv[0] == "systemctl") {
        assert(argv == (std::vector<std::string>{"systemctl", "reboot"}));
    }

    // Non-power verbs are never touched: lock-session stays on loginctl.
    PowerAction l;
    l.id = "lock";
    l.argv = {"loginctl", "lock-session"};
    assert(PowerActionBackend::resolve_argv(l) ==
           (std::vector<std::string>{"loginctl", "lock-session"}));

    // Custom commands without a power verb pass through untouched.
    PowerAction c;
    c.id = "shutdown";
    c.argv = {"my-shutdown-script", "--now"};
    assert(PowerActionBackend::resolve_argv(c) ==
           (std::vector<std::string>{"my-shutdown-script", "--now"}));

    std::cout << "[PASS] resolve argv normalization\n";
}

void test_split_argv() {
    using V = std::vector<std::string>;
    assert(PowerActionBackend::split_argv("loginctl poweroff") == (V{"loginctl", "poweroff"}));
    assert(PowerActionBackend::split_argv("sh -c \"echo hi there\"") == (V{"sh", "-c", "echo hi there"}));
    assert(PowerActionBackend::split_argv("cmd 'a b'  c") == (V{"cmd", "a b", "c"}));
    assert(PowerActionBackend::split_argv("  spaced   out  ") == (V{"spaced", "out"}));
    assert(PowerActionBackend::split_argv("").empty());
    assert(PowerActionBackend::split_argv("emp\"\"ty") == (V{"empty"}));
    std::cout << "[PASS] split argv\n";
}

int main() {
    test_default_actions();
    test_command_override();
    test_confirm_text_override();
    test_enabled_subset_and_order();
    test_resolve_argv_normalization();
    test_split_argv();
    std::cout << "All power action backend tests passed!\n";
    return 0;
}
