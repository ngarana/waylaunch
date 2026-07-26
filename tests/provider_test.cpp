// Unit tests for the ResultProvider seam (§5.2). Providers are pure enough to
// test off-Wayland: we exercise query() logic and activate() dispatch directly.
// activate()'s "handled" path has process side effects (spawn/clipboard), so we
// only assert its rejection path here — the query logic is what carries risk.

#include "waylaunch/providers/calculator_provider.h"
#include "waylaunch/providers/command_provider.h"
#include "waylaunch/config.h"    // Command (HistoryStore is forward-declared by the provider)

#include <cassert>
#include <cctype>
#include <string>
#include <vector>

using namespace waylaunch;

static ProviderQuery mkq(const std::string& text) {
    std::string lower = text;
    for (auto& c : lower) c = static_cast<char>(std::tolower((unsigned char)c));
    return ProviderQuery{text, lower, 6};
}

int main() {
    // --- CalculatorProvider: a valid expression is a single Top-Hit item. ---
    {
        CalculatorProvider calc;

        auto r = calc.query(mkq("6*7"));
        assert(r.size() == 1);
        assert(r[0].kind == ItemKind::Calculator);
        assert(!r[0].path.empty());               // the result payload (copied on Return)
        assert(r[0].name.rfind("= ", 0) == 0);    // rendered as "= 42"
        assert(r[0].score > 1000.0f);             // wins the Top Hit

        assert(calc.query(mkq("just some words")).empty());
        assert(calc.query(mkq("")).empty());

        // activate() only claims Calculator items.
        ListItem file;
        file.kind = ItemKind::File;
        file.path = "/tmp/whatever";
        assert(!calc.activate(file));
    }

    // --- CommandProvider: matches injected [[commands]] by name, no history. ---
    {
        std::vector<Command> cmds;
        Command lock;
        lock.name = "Lock Screen";
        lock.command = "loginctl lock-session";
        lock.category = "System";
        cmds.push_back(lock);
        Command sleepc;
        sleepc.name = "Sleep";
        sleepc.command = "systemctl suspend";
        cmds.push_back(sleepc);

        CommandProvider prov(&cmds, /*history=*/nullptr);

        auto r = prov.query(mkq("lock"));
        assert(r.size() == 1);
        assert(r[0].kind == ItemKind::Command);
        assert(r[0].name == "Lock Screen");
        assert(r[0].action_command == "loginctl lock-session");

        assert(prov.query(mkq("zzz-nomatch")).empty());
        assert(prov.query(mkq("SLEEP")).size() == 1);   // case-insensitive

        // activate() only claims Command items (no process is spawned here).
        ListItem app;
        app.kind = ItemKind::Application;
        assert(!prov.activate(app));
    }

    return 0;
}
