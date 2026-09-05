#include "waylaunch/providers/calculator_provider.h"
#include "waylaunch/calculator.h"
#include "waylaunch/clipboard.h"

namespace waylaunch {

std::vector<ListItem> CalculatorProvider::query(const ProviderQuery& q) {
    std::vector<ListItem> out;
    Calculator calc;
    auto r = calc.evaluate(q.text);
    if (r.valid && Calculator::is_calculator_query(q.text)) {
        ListItem it;
        it.kind = ItemKind::Calculator;
        it.name = "= " + r.result;
        it.description = q.text;
        it.icon_name = "accessories-calculator";
        it.path = r.result; // payload copied to clipboard on Return
        it.score = 1e6F;    // a valid expression is the Top Hit
        out.push_back(std::move(it));
    }
    return out;
}

bool CalculatorProvider::activate(const ListItem& it) {
    if (it.kind != ItemKind::Calculator) return false;
    if (!it.path.empty()) Clipboard::copy_text(it.path);
    return true;
}

} // namespace waylaunch
