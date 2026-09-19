#include "waylaunch/switcher/hyprland_focus.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace waylaunch {

std::string hypr_lua_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

bool hypr_address_is_safe(const std::string& address) {
    if (address.empty() || address.size() > 18) return false;
    size_t start = 0;
    if (address.size() > 2 && address[0] == '0' && (address[1] == 'x' || address[1] == 'X')) {
        start = 2;
    }
    if (start == address.size()) return false;
    for (size_t i = start; i < address.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(address[i]))) return false;
    }
    return true;
}

std::string hypr_focus_payload_lua(const std::string& address) {
    return "hl.dsp.focus({window=\"address:" + hypr_lua_escape(address) + "\"})";
}

std::string hypr_focus_payload_stock(const std::string& address) {
    return "focuswindow address:" + address;
}

std::optional<std::string> pick_hypr_address(const std::vector<HyprClient>& clients,
                                             const std::string& app_id, const std::string& title) {
    // 1. Exact class + title. First hit wins (handles are unique per window;
    // duplicates here mean the compositor reports the same window twice).
    for (const auto& c : clients) {
        if (!app_id.empty() && c.klass == app_id && !title.empty() && c.title == title) {
            if (!c.address.empty()) return c.address;
        }
    }
    // 2. Title-only exact match. Titles identify an instance even when the
    // wlr app_id and the Hyprland class disagree (XWayland skew).
    if (!title.empty()) {
        for (const auto& c : clients) {
            if (c.title == title && !c.address.empty()) return c.address;
        }
    }
    // 3. Class-only, but only when unambiguous: focusing a guess among
    // several same-class windows could land on the wrong workspace.
    if (!app_id.empty()) {
        const HyprClient* single = nullptr;
        int count = 0;
        for (const auto& c : clients) {
            if (c.klass == app_id) {
                ++count;
                single = &c;
            }
        }
        if (count == 1 && single != nullptr && !single->address.empty()) return single->address;
    }
    return std::nullopt;
}

std::string hypr_ipc_socket_path() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const char* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (runtime == nullptr || runtime[0] == '\0' || signature == nullptr || signature[0] == '\0') {
        return "";
    }
    return std::string(runtime) + "/hypr/" + signature + "/.socket.sock";
}

std::optional<std::string> hypr_ipc_request(const std::string& command) {
    std::string path = hypr_ipc_socket_path();
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) return std::nullopt;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return std::nullopt;
    // Bound the read so a wedged compositor cannot hang the Alt+Tab confirm.
    timeval timeout{.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    bool connected = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    std::string reply;
    if (connected) {
        size_t written = 0;
        while (written < command.size()) {
            ssize_t n = write(fd, command.data() + written, command.size() - written);
            if (n <= 0) {
                connected = false;
                break;
            }
            written += static_cast<size_t>(n);
        }
        if (connected) {
            shutdown(fd, SHUT_WR);
            char buf[4096];
            while (true) {
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n <= 0) break;
                reply.append(buf, static_cast<size_t>(n));
            }
        }
    }
    close(fd);
    if (!connected) return std::nullopt;
    return reply;
}

namespace {

bool dispatch_ok(const std::string& payload) {
    auto reply = hypr_ipc_request("/dispatch " + payload);
    return reply.has_value() && reply->starts_with("ok");
}

} // namespace

bool hypr_focus_address(const std::string& address) {
    if (!hypr_address_is_safe(address)) return false;
    // Lua-scripted setups first; stock Hyprland understands `focuswindow`.
    // Either miss is a safe no-op (never a fallback onto another window).
    if (dispatch_ok(hypr_focus_payload_lua(address))) return true;
    return dispatch_ok(hypr_focus_payload_stock(address));
}

bool hypr_focus_window(const std::string& app_id, const std::string& title) {
    if (hypr_ipc_socket_path().empty()) return false;
    auto reply = hypr_ipc_request("j/clients");
    if (!reply.has_value()) return false;
    auto address = pick_hypr_address(parse_hypr_clients(*reply), app_id, title);
    if (!address.has_value()) return false;
    return hypr_focus_address(*address);
}

} // namespace waylaunch
