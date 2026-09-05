#include "waylaunch/dropdown/hyprland_backend.h"

#include "waylaunch/dropdown/focus_guard.h"
#include "waylaunch/dropdown/hyprland_json.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace waylaunch {
namespace {

// Every command string in this file; none inline elsewhere (unversioned IPC).
constexpr const char* kClientsCmd = "j/clients";
constexpr const char* kMonitorsCmd = "j/monitors";

std::string socket_path() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const char* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (runtime == nullptr || runtime[0] == '\0' || signature == nullptr || signature[0] == '\0') {
        return "";
    }
    return std::string(runtime) + "/hypr/" + signature + "/.socket.sock";
}

// One-shot request: connect, write one command, read until EOF, close.
// Returns nullopt on any transport failure. SO_RCVTIMEO bounds the read so a
// wedged compositor cannot hang the daemon's poll loop.
std::optional<std::string> request(const std::string& command) {
    std::string path = socket_path();
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) return std::nullopt;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return std::nullopt;
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

// A reply counts as success only on `ok`. Misses are safe no-ops by
// construction (Spike 0): `warning:`/`info:` mean "no such window", never a
// fallback onto the active window, so callers treat them as failure and
// resync rather than assuming placement landed.
bool dispatch(const std::string& lua) {
    auto reply = request("/dispatch " + lua);
    return reply.has_value() && reply->starts_with("ok");
}

std::string lua_escape(const std::string& selector) {
    std::string out;
    out.reserve(selector.size());
    for (char c : selector) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

std::string window_selector(const WindowInfo& window) { return "address:" + window.address; }

std::string quoted(const WindowInfo& window) {
    return "\"" + lua_escape(window_selector(window)) + "\"";
}

} // namespace

std::optional<WindowInfo> HyprlandBackend::find_window(const std::string& app_id) {
    auto reply = request(kClientsCmd);
    if (!reply.has_value()) return std::nullopt;
    for (const HyprClient& client : parse_hypr_clients(*reply)) {
        if (client.klass != app_id) continue;
        WindowInfo info;
        info.address = client.address;
        info.app_id = app_id;
        info.pid = client.pid;
        info.geom =
            Geometry{.x = client.at_x, .y = client.at_y, .w = client.width, .h = client.height};
        info.workspace = client.workspace_id;
        info.visible = client.mapped && !client.workspace_name.starts_with("special:");
        return info;
    }
    return std::nullopt;
}

std::optional<WindowInfo> HyprlandBackend::find_by_address(const std::string& address) {
    auto reply = request(kClientsCmd);
    if (!reply.has_value()) return std::nullopt;
    std::string want = normalize_address(address);
    for (const HyprClient& client : parse_hypr_clients(*reply)) {
        if (normalize_address(client.address) != want) continue;
        WindowInfo info;
        info.address = client.address;
        info.app_id = client.klass;
        info.pid = client.pid;
        info.geom =
            Geometry{.x = client.at_x, .y = client.at_y, .w = client.width, .h = client.height};
        info.workspace = client.workspace_id;
        info.visible = client.mapped && !client.workspace_name.starts_with("special:");
        return info;
    }
    return std::nullopt;
}

std::optional<MonitorInfo> HyprlandBackend::focused_monitor() {
    auto reply = request(kMonitorsCmd);
    if (!reply.has_value()) return std::nullopt;
    std::vector<HyprMonitor> monitors = parse_hypr_monitors(*reply);
    if (monitors.empty()) return std::nullopt;
    const HyprMonitor* focused = monitors.data();
    for (const HyprMonitor& monitor : monitors) {
        if (monitor.focused) focused = &monitor;
    }
    MonitorInfo info;
    info.name = focused->name;
    info.x = focused->x;
    info.y = focused->y;
    info.w = focused->width;
    info.h = focused->height;
    info.scale = focused->scale;
    info.reserved_top = focused->reserved_top;
    info.reserved_bottom = focused->reserved_bottom;
    info.active_workspace = focused->active_workspace;
    return info;
}

bool HyprlandBackend::show(const WindowInfo& window, const Geometry& geometry) {
    auto monitor = focused_monitor();
    if (!monitor.has_value()) return false;
    std::string target = quoted(window);
    // Order matters: float first (tiled windows ignore pixel moves), then
    // workspace with follow (focus tracks the dropdown on show), then resize
    // BEFORE the final move — Hyprland's resize preserves the window center,
    // so moving first and resizing after drifts the position whenever the
    // size changes (verified live). Raise and explicit focus close any race.
    if (!dispatch("hl.dsp.window.float({window=" + target + ", action=\"set\"})")) return false;
    std::string workspace = std::to_string(monitor->active_workspace);
    if (!dispatch("hl.dsp.window.move({window=" + target + ", workspace=\"" + workspace + "\"})")) {
        return false;
    }
    if (!dispatch("hl.dsp.window.resize({window=" + target + ", x=" + std::to_string(geometry.w) +
                  ", y=" + std::to_string(geometry.h) + "})")) {
        return false;
    }
    if (!dispatch("hl.dsp.window.move({window=" + target + ", x=" + std::to_string(geometry.x) +
                  ", y=" + std::to_string(geometry.y) + "})")) {
        return false;
    }
    if (!dispatch("hl.dsp.window.alter_zorder({window=" + target + ", mode=\"top\"})")) {
        return false;
    }
    return dispatch("hl.dsp.focus({window=" + target + "})");
}

bool HyprlandBackend::hide(const WindowInfo& window) {
    // Silent workspace move: follow=false keeps focus where it is, so hiding
    // never steals focus (Spike 0). Geometry is reapplied on every show, so
    // nothing needs persisting here.
    std::string target = quoted(window);
    return dispatch("hl.dsp.window.move({window=" + target + ", workspace=\"" + hidden_workspace() +
                    "\", follow=false})");
}

bool HyprlandBackend::focus(const WindowInfo& window) {
    return dispatch("hl.dsp.focus({window=" + quoted(window) + "})");
}

bool HyprlandBackend::supports_geometry() const {
    return !socket_path().empty() && request("j/monitors").has_value();
}

} // namespace waylaunch
