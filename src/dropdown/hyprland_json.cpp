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

// Reads 4 hex digits as a code unit; -1 when fewer than 4 remain.
int parse_hex4(Cursor& cur) {
    int value = 0;
    for (int i = 0; i < 4; ++i) {
        if (cur.p >= cur.end || !std::isxdigit(static_cast<unsigned char>(*cur.p))) return -1;
        char c = *cur.p++;
        int digit = (c <= '9') ? (c - '0') : ((c | 0x20) - 'a' + 10);
        value = (value << 4) | digit;
    }
    return value;
}

void append_utf8(std::string& out, unsigned int cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Parses a JSON string including \" \\ \/ \b \f \n \r \t and \uXXXX.
// Escapes decode to real UTF-8 (surrogate pairs included): titles are drawn
// in the tab strip, so a terminal titled with non-ASCII must survive intact.
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
            case 'u': {
                int unit = parse_hex4(cur);
                if (unit < 0) return false;
                unsigned int cp = static_cast<unsigned int>(unit);
                // A high surrogate must be followed by \uDC00-\uDFFF; anything
                // else is emitted as U+FFFD rather than a malformed sequence.
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    int low = -1;
                    if (cur.end - cur.p >= 2 && cur.p[0] == '\\' && cur.p[1] == 'u') {
                        const char* save = cur.p;
                        cur.p += 2;
                        low = parse_hex4(cur);
                        if (low < 0xDC00 || low > 0xDFFF) {
                            cur.p = save;
                            low = -1;
                        }
                    }
                    cp = (low < 0) ? 0xFFFDU
                                   : 0x10000U + ((cp - 0xD800U) << 10) +
                                         (static_cast<unsigned int>(low) - 0xDC00U);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD; // lone low surrogate
                }
                append_utf8(out, cp);
                break;
            }
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
            } else if (key == "title") {
                ok = parse_string(cur, client.title);
            } else if (key == "focusHistoryID") {
                double value = 0;
                ok = parse_number(cur, value);
                client.focus_history_id = static_cast<int>(value);
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
