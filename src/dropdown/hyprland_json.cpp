#include "waylaunch/dropdown/hyprland_json.h"

#include <cctype>
#include <cstdlib>

namespace waylaunch {
namespace {

struct Cursor {
    const char* p = nullptr;
    const char* end = nullptr;

    bool empty() const { return p >= end; }
    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool consume(char c) {
        skip_ws();
        if (p < end && *p == c) {
            ++p;
            return true;
        }
        return false;
    }
    bool consume_literal(const char* word) {
        skip_ws();
        for (const char* q = word; *q != '\0'; ++q) {
            if (p >= end || *p != *q) return false;
            ++p;
        }
        return true;
    }
};

// Parses a JSON string including \" \\ \/ \b \f \n \r \t and \uXXXX
// (non-ASCII escapes degrade to '?'; titles are never matched on, only
// skipped, so fidelity beyond ASCII is unnecessary).
bool parse_string(Cursor& cur, std::string& out) {
    cur.skip_ws();
    if (cur.empty() || *cur.p != '"') return false;
    ++cur.p;
    out.clear();
    while (cur.p < cur.end) {
        char c = *cur.p++;
        if (c == '"') return true;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (cur.p >= cur.end) return false;
        char esc = *cur.p++;
        switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
                for (int i = 0; i < 4; ++i) {
                    if (cur.p >= cur.end || !std::isxdigit(static_cast<unsigned char>(*cur.p))) {
                        return false;
                    }
                    ++cur.p;
                }
                out.push_back('?');
                break;
            default: return false;
        }
    }
    return false;
}

bool parse_bool(Cursor& cur, bool& out) {
    if (cur.consume_literal("true")) {
        out = true;
        return true;
    }
    if (cur.consume_literal("false")) {
        out = false;
        return true;
    }
    return false;
}

// strtod-based so integers, negatives, fractions, and exponents all parse.
bool parse_number(Cursor& cur, double& out) {
    cur.skip_ws();
    if (cur.empty()) return false;
    char* stop = nullptr;
    // strtod stops at the first invalid char; require it to consume something.
    double value = std::strtod(cur.p, &stop);
    if (stop == cur.p || stop > cur.end) return false;
    cur.p = stop;
    out = value;
    return true;
}

// Skips any JSON value: strings (brace-containing titles included), numbers,
// literals, arrays, and nested objects.
bool skip_value(Cursor& cur) {
    cur.skip_ws();
    if (cur.empty()) return false;
    if (*cur.p == '"') {
        std::string ignored;
        return parse_string(cur, ignored);
    }
    if (*cur.p == '{' || *cur.p == '[') {
        char open = *cur.p++;
        char close = (open == '{') ? '}' : ']';
        int depth = 1;
        while (cur.p < cur.end && depth > 0) {
            if (*cur.p == '"') {
                std::string ignored;
                if (!parse_string(cur, ignored)) return false;
            } else {
                if (*cur.p == open) ++depth;
                if (*cur.p == close) --depth;
                ++cur.p;
            }
        }
        return depth == 0;
    }
    if (*cur.p == 't' || *cur.p == 'f' || *cur.p == 'n') {
        return cur.consume_literal("true") || cur.consume_literal("false") ||
               cur.consume_literal("null");
    }
    double ignored = 0;
    return parse_number(cur, ignored);
}

bool parse_int_pair(Cursor& cur, int& first, int& second) {
    if (!cur.consume('[')) return false;
    double a = 0;
    double b = 0;
    if (!parse_number(cur, a) || !cur.consume(',') || !parse_number(cur, b) || !cur.consume(']')) {
        return false;
    }
    first = static_cast<int>(a);
    second = static_cast<int>(b);
    return true;
}

bool parse_reserved(Cursor& cur, int& top, int& bottom) {
    // Hyprland emits reserved as [left, top, right, bottom] (see
    // src/ipc/s1/Commands.cpp monitors output). Only the vertical bar
    // insets matter for top/bottom-anchored placement.
    if (!cur.consume('[')) return false;
    double values[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        if (i > 0 && !cur.consume(',')) return false;
        if (!parse_number(cur, values[i])) return false;
    }
    if (!cur.consume(']')) return false;
    top = static_cast<int>(values[1]);
    bottom = static_cast<int>(values[3]);
    return true;
}

bool parse_workspace(Cursor& cur, int& id, std::string& name) {
    if (!cur.consume('{')) return false;
    while (true) {
        cur.skip_ws();
        if (cur.consume('}')) return true;
        std::string key;
        if (!parse_string(cur, key) || !cur.consume(':')) return false;
        if (key == "id") {
            double value = 0;
            if (!parse_number(cur, value)) return false;
            id = static_cast<int>(value);
        } else if (key == "name") {
            if (!parse_string(cur, name)) return false;
        } else if (!skip_value(cur)) {
            return false;
        }
        cur.skip_ws();
        if (cur.consume(',')) continue;
        if (cur.consume('}')) return true;
        return false;
    }
}

bool parse_active_workspace(Cursor& cur, int& id) {
    std::string ignored;
    return parse_workspace(cur, id, ignored);
}

} // namespace

std::vector<HyprClient> parse_hypr_clients(const std::string& json) {
    std::vector<HyprClient> clients;
    Cursor cur{.p = json.data(), .end = json.data() + json.size()};
    if (!cur.consume('[')) return clients;
    cur.skip_ws();
    if (cur.consume(']')) return clients;
    while (true) {
        HyprClient client;
        if (!cur.consume('{')) return clients;
        while (true) {
            cur.skip_ws();
            if (cur.consume('}')) break;
            std::string key;
            if (!parse_string(cur, key) || !cur.consume(':')) return clients;
            bool ok = true;
            if (key == "address") {
                ok = parse_string(cur, client.address);
            } else if (key == "class") {
                ok = parse_string(cur, client.klass);
            } else if (key == "pid") {
                double value = 0;
                ok = parse_number(cur, value);
                client.pid = static_cast<int>(value);
            } else if (key == "at") {
                ok = parse_int_pair(cur, client.at_x, client.at_y);
            } else if (key == "size") {
                ok = parse_int_pair(cur, client.width, client.height);
            } else if (key == "workspace") {
                ok = parse_workspace(cur, client.workspace_id, client.workspace_name);
            } else if (key == "monitor") {
                double value = 0;
                ok = parse_number(cur, value);
                client.monitor = static_cast<int>(value);
            } else if (key == "floating") {
                ok = parse_bool(cur, client.floating);
            } else if (key == "mapped") {
                ok = parse_bool(cur, client.mapped);
            } else {
                ok = skip_value(cur);
            }
            if (!ok) return clients;
            cur.skip_ws();
            if (cur.consume(',')) continue;
            if (cur.consume('}')) break;
            return clients;
        }
        clients.push_back(std::move(client));
        cur.skip_ws();
        if (cur.consume(',')) continue;
        if (!cur.consume(']')) return clients;
        return clients;
    }
    return clients;
}

std::vector<HyprMonitor> parse_hypr_monitors(const std::string& json) {
    std::vector<HyprMonitor> monitors;
    Cursor cur{.p = json.data(), .end = json.data() + json.size()};
    if (!cur.consume('[')) return monitors;
    cur.skip_ws();
    if (cur.consume(']')) return monitors;
    while (true) {
        HyprMonitor monitor;
        if (!cur.consume('{')) return monitors;
        while (true) {
            cur.skip_ws();
            if (cur.consume('}')) break;
            std::string key;
            if (!parse_string(cur, key) || !cur.consume(':')) return monitors;
            bool ok = true;
            if (key == "name") {
                ok = parse_string(cur, monitor.name);
            } else if (key == "x" || key == "y" || key == "width" || key == "height") {
                double value = 0;
                ok = parse_number(cur, value);
                int ivalue = static_cast<int>(value);
                if (key == "x") {
                    monitor.x = ivalue;
                } else if (key == "y") {
                    monitor.y = ivalue;
                } else if (key == "width") {
                    monitor.width = ivalue;
                } else {
                    monitor.height = ivalue;
                }
            } else if (key == "scale") {
                ok = parse_number(cur, monitor.scale);
            } else if (key == "focused") {
                ok = parse_bool(cur, monitor.focused);
            } else if (key == "activeWorkspace") {
                ok = parse_active_workspace(cur, monitor.active_workspace);
            } else if (key == "reserved") {
                ok = parse_reserved(cur, monitor.reserved_top, monitor.reserved_bottom);
            } else {
                ok = skip_value(cur);
            }
            if (!ok) return monitors;
            cur.skip_ws();
            if (cur.consume(',')) continue;
            if (cur.consume('}')) break;
            return monitors;
        }
        monitors.push_back(std::move(monitor));
        cur.skip_ws();
        if (cur.consume(',')) continue;
        if (!cur.consume(']')) return monitors;
        return monitors;
    }
    return monitors;
}

} // namespace waylaunch
