#include "waylaunch/wayland_core.h"
#include <algorithm>
#include <cairo/cairo.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

namespace {
bool wl_dbg() {
    static bool v = std::getenv("WAYLAUNCH_DEBUG") != nullptr;
    return v;
}
} // namespace

namespace {
int create_unlinked_shm_file() {
    char name[] = "/tmp/waylaunch-shm-XXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) return -1;
    if (unlink(name) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}
} // namespace

extern "C" {
#include "wlr-layer-shell-client-protocol.h"
#ifdef HAS_SCREENCOPY
#include "wlr-screencopy-client-protocol.h"
#endif
#ifdef HAS_FOREIGN_TOPLEVEL
#include "wlr-foreign-toplevel-management-client-protocol.h"
#endif
}

namespace waylaunch {

// --- Buffer ---
Buffer::~Buffer() { destroy(); }

void Buffer::destroy() {
    if (wl_buf) {
        wl_buffer_destroy(wl_buf);
        wl_buf = nullptr;
    }
    if (pool) {
        wl_shm_pool_destroy(pool);
        pool = nullptr;
    }
    if (data && size > 0) {
        munmap(data, size);
        data = nullptr;
    }
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    busy = false;
}

Buffer::Buffer(Buffer&& o) noexcept
    : fd(o.fd), data(o.data), wl_buf(o.wl_buf), pool(o.pool), width(o.width), height(o.height),
      stride(o.stride), size(o.size), busy(o.busy) {
    o.fd = -1;
    o.data = nullptr;
    o.wl_buf = nullptr;
    o.pool = nullptr;
    o.width = o.height = o.stride = o.size = 0;
    o.busy = false;
}

Buffer& Buffer::operator=(Buffer&& o) noexcept {
    if (this != &o) {
        destroy();
        fd = o.fd;
        data = o.data;
        wl_buf = o.wl_buf;
        pool = o.pool;
        width = o.width;
        height = o.height;
        stride = o.stride;
        size = o.size;
        busy = o.busy;
        o.fd = -1;
        o.data = nullptr;
        o.wl_buf = nullptr;
        o.pool = nullptr;
        o.width = o.height = o.stride = o.size = 0;
        o.busy = false;
    }
    return *this;
}

// --- KeyboardState ---
KeyboardState::KeyboardState() { xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS); }
KeyboardState::~KeyboardState() {
    if (state) xkb_state_unref(state);
    if (keymap) xkb_keymap_unref(keymap);
    if (xkb_ctx) xkb_context_unref(xkb_ctx);
}

// --- Trampolines ---

static void registry_global_cb(void* data, wl_registry* reg, uint32_t name, const char* interface,
                               uint32_t version);
static void registry_global_remove_cb(void* data, wl_registry* reg, uint32_t name);
static const wl_registry_listener registry_listener = {.global = registry_global_cb,
                                                       .global_remove = registry_global_remove_cb};

// Buffer release
static void wl_buffer_release_cb(void* data, wl_buffer* buf) {
    static_cast<WaylandCore*>(data)->handle_buffer_release(buf);
}
static const wl_buffer_listener wl_buffer_listener = {.release = wl_buffer_release_cb};

// Keyboard
static void keyboard_keymap_cb(void* data, wl_keyboard*, uint32_t format, int32_t fd,
                               uint32_t size) {
    static_cast<WaylandCore*>(data)->handle_keymap(format, fd, size);
}
static void keyboard_enter_cb(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
static void keyboard_leave_cb(void*, wl_keyboard*, uint32_t, wl_surface*) {}
static void keyboard_key_cb(void* data, wl_keyboard*, uint32_t serial, uint32_t time, uint32_t key,
                            uint32_t state) {
    static_cast<WaylandCore*>(data)->handle_key(serial, time, key, state);
}
static void keyboard_modifiers_cb(void* data, wl_keyboard*, uint32_t, uint32_t md, uint32_t ml,
                                  uint32_t mk, uint32_t g) {
    static_cast<WaylandCore*>(data)->handle_modifiers(md, ml, mk, g);
}
static void keyboard_repeat_info_cb(void* data, wl_keyboard*, int32_t rate, int32_t delay) {
    auto* self = static_cast<WaylandCore*>(data);
    self->kbd_.repeat_rate = rate;
    self->kbd_.repeat_delay = delay;
}
static const wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap_cb,
    .enter = keyboard_enter_cb,
    .leave = keyboard_leave_cb,
    .key = keyboard_key_cb,
    .modifiers = keyboard_modifiers_cb,
    .repeat_info = keyboard_repeat_info_cb,
};

// Pointer
static void pointer_enter_cb(void* data, wl_pointer*, uint32_t serial, wl_surface*, wl_fixed_t x,
                             wl_fixed_t y) {
    auto* self = static_cast<WaylandCore*>(data);
    self->pointer_x_ = wl_fixed_to_double(x);
    self->pointer_y_ = wl_fixed_to_double(y);
    self->ensure_cursor(serial); // paint a visible pointer over our surface
    if (self->mouse_move_handler_) self->mouse_move_handler_(self->pointer_x_, self->pointer_y_);
}
static void pointer_leave_cb(void*, wl_pointer*, uint32_t, wl_surface*) {}
static void pointer_motion_cb(void* data, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    auto* self = static_cast<WaylandCore*>(data);
    self->pointer_x_ = wl_fixed_to_double(x);
    self->pointer_y_ = wl_fixed_to_double(y);
    if (self->mouse_move_handler_) self->mouse_move_handler_(self->pointer_x_, self->pointer_y_);
}
static void pointer_button_cb(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button,
                              uint32_t state) {
    auto* self = static_cast<WaylandCore*>(data);
    if (self->mouse_handler_)
        self->mouse_handler_(self->pointer_x_, self->pointer_y_, button,
                             state == WL_POINTER_BUTTON_STATE_PRESSED);
}
static void pointer_axis_cb(void* data, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
    auto* self = static_cast<WaylandCore*>(data);
    if (self->axis_handler_)
        self->axis_handler_(self->pointer_x_, self->pointer_y_, static_cast<int32_t>(axis),
                            wl_fixed_to_double(value));
}
static void pointer_frame_cb(void*, wl_pointer*) {}
static void pointer_axis_source_cb(void*, wl_pointer*, uint32_t) {}
static void pointer_axis_stop_cb(void*, wl_pointer*, uint32_t, uint32_t) {}
static void pointer_axis_discrete_cb(void*, wl_pointer*, uint32_t, int32_t) {}
static void pointer_axis_value120_cb(void*, wl_pointer*, uint32_t, int32_t) {}
static void pointer_axis_relative_direction_cb(void*, wl_pointer*, uint32_t, uint32_t) {}
static const wl_pointer_listener pointer_listener = {
    .enter = pointer_enter_cb,
    .leave = pointer_leave_cb,
    .motion = pointer_motion_cb,
    .button = pointer_button_cb,
    .axis = pointer_axis_cb,
    .frame = pointer_frame_cb,
    .axis_source = pointer_axis_source_cb,
    .axis_stop = pointer_axis_stop_cb,
    .axis_discrete = pointer_axis_discrete_cb,
    .axis_value120 = pointer_axis_value120_cb,
    .axis_relative_direction = pointer_axis_relative_direction_cb,
};

// Seat
static void seat_capabilities_cb(void* data, wl_seat* seat, uint32_t caps) {
    auto* self = static_cast<WaylandCore*>(data);
    if (wl_dbg())
        fprintf(stderr, "[wl] seat caps=%u (kbd=%d ptr=%d)\n", caps,
                !!(caps & WL_SEAT_CAPABILITY_KEYBOARD), !!(caps & WL_SEAT_CAPABILITY_POINTER));
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !self->keyboard_) {
        self->keyboard_ = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(self->keyboard_, &keyboard_listener, self);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !self->pointer_) {
        self->pointer_ = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(self->pointer_, &pointer_listener, self);
    }
}
static void seat_name_cb(void*, wl_seat*, const char*) {}
static const wl_seat_listener seat_listener = {.capabilities = seat_capabilities_cb,
                                               .name = seat_name_cb};

// Output
static void output_geometry_cb(void* data, wl_output*, int32_t x, int32_t y, int32_t w, int32_t h,
                               int32_t, const char*, const char*, int32_t) {
    static_cast<WaylandCore*>(data)->handle_output_geometry(x, y, w, h, 0, 1);
}
static void output_mode_cb(void* data, wl_output*, uint32_t flags, int32_t w, int32_t h,
                           int32_t r) {
    static_cast<WaylandCore*>(data)->handle_output_mode(flags, w, h, r);
}
static void output_done_cb(void*, wl_output*) {}
static void output_scale_cb(void* data, wl_output*, int32_t f) {
    static_cast<WaylandCore*>(data)->handle_output_scale(f);
}
static void output_name_cb(void* data, wl_output*, const char* n) {
    static_cast<WaylandCore*>(data)->handle_output_name(n ? n : "");
}
static void output_description_cb(void*, wl_output*, const char*) {}
static const wl_output_listener output_listener = {
    .geometry = output_geometry_cb,
    .mode = output_mode_cb,
    .done = output_done_cb,
    .scale = output_scale_cb,
    .name = output_name_cb,
    .description = output_description_cb,
};

// Registry
static void registry_global_cb(void* data, wl_registry* reg, uint32_t name, const char* interface,
                               uint32_t) {
    auto* self = static_cast<WaylandCore*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        self->compositor_ =
            static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        self->shm_ = static_cast<wl_shm*>(wl_registry_bind(reg, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        self->seat_ = static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, 1));
        wl_seat_add_listener(self->seat_, &seat_listener, self);
    } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
        auto* output =
            static_cast<wl_output*>(wl_registry_bind(reg, name, &wl_output_interface, 4));
        self->outputs_.push_back(
            {.output = output, .width = 0, .height = 0, .scale = 1, .name = ""});
        wl_output_add_listener(output, &output_listener, self);
    } else if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        self->layer_shell_ = static_cast<zwlr_layer_shell_v1*>(
            wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1));
    }
#ifdef HAS_SCREENCOPY
    else if (std::strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        self->screencopy_manager_ = static_cast<zwlr_screencopy_manager_v1*>(
            wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, 1));
    }
#endif
#ifdef HAS_FOREIGN_TOPLEVEL
    else if (std::strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        self->foreign_toplevel_manager_ = static_cast<zwlr_foreign_toplevel_manager_v1*>(
            wl_registry_bind(reg, name, &zwlr_foreign_toplevel_manager_v1_interface, 1));
        if (self->foreign_toplevel_listener_) {
            self->foreign_toplevel_listener_(self->foreign_toplevel_manager_);
        }
    }
#endif
}

static void registry_global_remove_cb(void*, wl_registry*, uint32_t) {}

// --- WaylandCore ---
WaylandCore::WaylandCore() = default;

WaylandCore::~WaylandCore() {
    buffers_.clear();
#ifdef HAS_SCREENCOPY
    if (screencopy_manager_) zwlr_screencopy_manager_v1_destroy(screencopy_manager_);
#endif
    if (layer_surface_) zwlr_layer_surface_v1_destroy(layer_surface_);
    if (layer_shell_) zwlr_layer_shell_v1_destroy(layer_shell_);
    if (surface_) wl_surface_destroy(surface_);
    if (cursor_surface_) wl_surface_destroy(cursor_surface_);
    if (cursor_theme_) wl_cursor_theme_destroy(cursor_theme_);
    if (pointer_) wl_pointer_destroy(pointer_);
    if (keyboard_) wl_keyboard_destroy(keyboard_);
    if (seat_) wl_seat_destroy(seat_);
    if (shm_) wl_shm_destroy(shm_);
    if (compositor_) wl_compositor_destroy(compositor_);
    if (registry_) wl_registry_destroy(registry_);
    if (display_) wl_display_disconnect(display_);
}

bool WaylandCore::init() {
    display_ = wl_display_connect(nullptr);
    if (!display_) return false;

    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registry_listener, this);
    wl_display_roundtrip(display_);

    if (!compositor_ || !shm_) return false;

    surface_ = wl_compositor_create_surface(compositor_);
    if (!surface_) return false;

    // Grab the current desktop into an SHM buffer BEFORE our overlay is mapped,
    // so the launcher can blur it as a frosted-glass backdrop. Best-effort:
    // failure just means no glass (opaque panel).
    if (want_backdrop_) capture_backdrop();

    if (!layer_shell_) {
        std::fprintf(stderr, "waylaunch: compositor does not provide wlr-layer-shell\n");
        return false;
    }

    // Full-output transparent overlay. The panel is drawn near the top and the
    // rest of the surface remains transparent so clicks outside it can dismiss
    // the launcher while the layer owns the keyboard exclusively.
    layer_surface_ = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell_, surface_, nullptr, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "waylaunch");
    if (!layer_surface_) return false;
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        layer_surface_, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_set_anchor(
        layer_surface_, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(layer_surface_, 0, 0);
    // -1: render above other exclusive zones (bars).
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface_, -1);

    static const zwlr_layer_surface_v1_listener ls_listener = {
        .configure =
            [](void* data, zwlr_layer_surface_v1*, uint32_t serial, int32_t w, int32_t h) {
                auto* self = static_cast<WaylandCore*>(data);
                if (w > 0) self->pending_width_ = w;
                if (h > 0) self->pending_height_ = h;
                self->configured_ = true;
                zwlr_layer_surface_v1_ack_configure(self->layer_surface_, serial);
                if (self->redraw_handler_) self->redraw_handler_();
            },
        .closed =
            [](void* data, zwlr_layer_surface_v1*) {
                auto* self = static_cast<WaylandCore*>(data);
                if (self->close_handler_) self->close_handler_();
                self->running_ = false;
            },
    };
    zwlr_layer_surface_v1_add_listener(layer_surface_, &ls_listener, this);

    pending_width_ = output_width();
    pending_height_ = output_height();

    wl_surface_commit(surface_);
    return true;
}

void WaylandCore::run() {
    running_ = true;
    while (running_ && wl_display_dispatch(display_) != -1) {}
}

void WaylandCore::set_running(bool v) { running_ = v; }
void WaylandCore::set_want_backdrop(bool v) { want_backdrop_ = v; }

void WaylandCore::quit() { running_ = false; }
bool WaylandCore::is_running() const { return running_; }
bool WaylandCore::is_configured() const { return configured_; }

OutputInfo& WaylandCore::primary_output() { return outputs_.front(); }
int32_t WaylandCore::primary_scale() const { return outputs_.empty() ? 1 : outputs_.front().scale; }
int32_t WaylandCore::output_width() const {
    return outputs_.empty() ? 1920 : outputs_.front().width;
}
int32_t WaylandCore::output_height() const {
    return outputs_.empty() ? 1080 : outputs_.front().height;
}
int32_t WaylandCore::surface_width() const {
    return pending_width_ > 0 ? pending_width_ : output_width();
}
int32_t WaylandCore::surface_height() const {
    return pending_height_ > 0 ? pending_height_ : output_height();
}
wl_display* WaylandCore::display() const { return display_; }
wl_surface* WaylandCore::surface() const { return surface_; }
wl_seat* WaylandCore::seat() const { return seat_; }

bool WaylandCore::modifier_active(const char* xkb_mod_name) const {
    return kbd_.state &&
           xkb_state_mod_name_is_active(kbd_.state, xkb_mod_name, XKB_STATE_MODS_EFFECTIVE);
}

Buffer* WaylandCore::acquire_buffer() {
    for (auto& buf : buffers_) {
        if (!buf->busy) {
            buf->busy = true;
            return buf.get();
        }
    }

    // Allocate new buffer
    int w = pending_width_ > 0 ? pending_width_ : 800;
    int h = pending_height_ > 0 ? pending_height_ : 500;

    auto buf = std::make_unique<Buffer>();
    buf->width = w;
    buf->height = h;
    buf->stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, w);
    buf->size = buf->stride * h;

    // mkstemp supplies the randomness that the old literal shm name lacked.
    // The file is unlinked immediately, but remains alive until the Wayland
    // buffer and its mmap are destroyed.
    buf->fd = create_unlinked_shm_file();
    if (buf->fd < 0) return nullptr;
    if (ftruncate(buf->fd, buf->size) < 0) {
        buf->destroy();
        return nullptr;
    }

    buf->data = static_cast<uint8_t*>(
        mmap(nullptr, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, buf->fd, 0));
    if (buf->data == MAP_FAILED) {
        buf->data = nullptr;
        buf->destroy();
        return nullptr;
    }

    buf->pool = wl_shm_create_pool(shm_, buf->fd, buf->size);
    if (!buf->pool) {
        buf->destroy();
        return nullptr;
    }
    buf->wl_buf =
        wl_shm_pool_create_buffer(buf->pool, 0, w, h, buf->stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(buf->pool);
    buf->pool = nullptr;
    if (!buf->wl_buf) {
        buf->destroy();
        return nullptr;
    }

    wl_buffer_add_listener(buf->wl_buf, &wl_buffer_listener, this);

    buf->busy = true;
    Buffer* result = buf.get();
    buffers_.push_back(std::move(buf));
    return result;
}

void WaylandCore::submit_buffer(Buffer* buf, int x, int y) {
    if (!buf || !buf->wl_buf) return;
    wl_surface_attach(surface_, buf->wl_buf, x, y);
    wl_surface_damage_buffer(surface_, 0, 0, buf->width, buf->height);
    wl_surface_commit(surface_);
}

void WaylandCore::unmap_surface() {
    if (!surface_) return;
    // A null buffer unmaps the surface: it leaves the screen and the compositor
    // drops its keyboard focus, so the user's windows get the keyboard back.
    // Per wlr-layer-shell, unmapping DISCARDS the layer_surface state and the
    // surface reverts to just-created: it must not get a buffer again until the
    // remap_surface() handshake has produced a fresh configure.
    wl_surface_attach(surface_, nullptr, 0, 0);
    wl_surface_commit(surface_);
    configured_ = false;
}

void WaylandCore::remap_surface() {
    if (!surface_ || !layer_surface_) return;
    // Re-mapping after an unmap = the initial-commit handshake all over again:
    // the unmap discarded anchor/size/interactivity, so re-apply them (same
    // values as init()), then commit WITHOUT a buffer. The compositor answers
    // with a configure; its handler acks and fires the redraw handler, whose
    // render attaches the first buffer and thereby maps + regains the keyboard.
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        layer_surface_, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_set_anchor(
        layer_surface_, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(layer_surface_, 0, 0);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface_, -1);
    wl_surface_commit(surface_);
}

void WaylandCore::handle_buffer_release(wl_buffer* wl_buf) {
    for (auto& buf : buffers_) {
        if (buf->wl_buf == wl_buf) {
            buf->busy = false;
            break;
        }
    }
}

// Paint the pointer over our own surface. A wlr-layer-shell surface with
// exclusive keyboard/pointer focus doesn't inherit the compositor's cursor, so
// without this the pointer is invisible while the overlay is up. Loads the
// user's XCURSOR theme/size once, then re-attaches the image on every enter
// (the `serial` must be the current enter's).
void WaylandCore::ensure_cursor(uint32_t serial) {
    if (!pointer_ || !compositor_ || !shm_) return;

    if (!cursor_theme_) {
        const char* size_env = std::getenv("XCURSOR_SIZE");
        int size = 24;
        if (size_env) {
            char* end = nullptr;
            long parsed = std::strtol(size_env, &end, 10);
            if (end != size_env) { size = static_cast<int>(parsed); }
        }
        if (size <= 0) size = 24;
        const char* theme_env = std::getenv("XCURSOR_THEME"); // nullptr = default theme
        cursor_theme_ = wl_cursor_theme_load(theme_env, size, shm_);
        if (cursor_theme_) {
            cursor_ = wl_cursor_theme_get_cursor(cursor_theme_, "default");
            if (!cursor_) cursor_ = wl_cursor_theme_get_cursor(cursor_theme_, "left_ptr");
        }
        if (!cursor_surface_) cursor_surface_ = wl_compositor_create_surface(compositor_);
        if (wl_dbg())
            fprintf(stderr, "[wl] cursor theme=%p cursor=%p surface=%p\n",
                    reinterpret_cast<void*>(cursor_theme_), reinterpret_cast<void*>(cursor_),
                    reinterpret_cast<void*>(cursor_surface_));
    }

    if (!cursor_ || !cursor_surface_ || cursor_->image_count == 0) {
        // No theme available: hide the cursor rather than leave it undefined.
        wl_pointer_set_cursor(pointer_, serial, nullptr, 0, 0);
        return;
    }

    wl_cursor_image* img = cursor_->images[0];
    wl_buffer* buf = wl_cursor_image_get_buffer(img);
    wl_pointer_set_cursor(pointer_, serial, cursor_surface_, static_cast<int32_t>(img->hotspot_x),
                          static_cast<int32_t>(img->hotspot_y));
    wl_surface_attach(cursor_surface_, buf, 0, 0);
    wl_surface_damage(cursor_surface_, 0, 0, static_cast<int32_t>(img->width),
                      static_cast<int32_t>(img->height));
    wl_surface_commit(cursor_surface_);
}

void WaylandCore::set_key_handler(KeyHandler h) { key_handler_ = std::move(h); }
void WaylandCore::set_modifiers_handler(ModifiersHandler h) { modifiers_handler_ = std::move(h); }
void WaylandCore::set_mouse_handler(MouseHandler h) { mouse_handler_ = std::move(h); }
void WaylandCore::set_mouse_move_handler(MouseMoveHandler h) { mouse_move_handler_ = std::move(h); }
void WaylandCore::set_axis_handler(AxisHandler h) { axis_handler_ = std::move(h); }
void WaylandCore::set_close_handler(CloseHandler h) { close_handler_ = std::move(h); }
void WaylandCore::set_redraw_handler(RedrawHandler h) { redraw_handler_ = std::move(h); }

void WaylandCore::handle_keymap(uint32_t format, int32_t fd, uint32_t size) {
    if (wl_dbg()) fprintf(stderr, "[wl] keymap format=%u size=%u\n", format, size);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    char* map = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (kbd_.keymap) xkb_keymap_unref(kbd_.keymap);
    kbd_.keymap = xkb_keymap_new_from_string(kbd_.xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                             XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);
    if (kbd_.state) xkb_state_unref(kbd_.state);
    kbd_.state = xkb_state_new(kbd_.keymap);
}

void WaylandCore::handle_key(uint32_t, uint32_t time, uint32_t key, uint32_t state) {
    if (wl_dbg())
        fprintf(stderr, "[wl] key code=%u state=%u (xkb_state=%p)\n", key, state,
                reinterpret_cast<void*>(kbd_.state));
    if (!kbd_.state) return;
    xkb_keysym_t keysym = xkb_state_key_get_one_sym(kbd_.state, key + 8);
    uint32_t utf32 = xkb_state_key_get_utf32(kbd_.state, key + 8);

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        // Store for repeat
        repeat_keysym_ = keysym;
        repeat_utf32_ = utf32;
        repeat_time_ = time;
        repeat_active_ = true;
        if (key_handler_) key_handler_(keysym, utf32, true);
    } else {
        repeat_active_ = false;
    }
}

void WaylandCore::handle_modifiers(uint32_t md, uint32_t ml, uint32_t mk, uint32_t g) const {
    if (!kbd_.state) return;
    xkb_state_update_mask(kbd_.state, md, ml, mk, 0, 0, g);
    if (modifiers_handler_) modifiers_handler_(md);
}

void WaylandCore::handle_output_geometry(int32_t, int32_t, int32_t w, int32_t h, int32_t, int32_t) {
    if (!outputs_.empty()) {
        outputs_.back().width = w;
        outputs_.back().height = h;
    }
}
void WaylandCore::handle_output_mode(uint32_t, int32_t w, int32_t h, int32_t) {
    if (!outputs_.empty()) {
        outputs_.back().width = w;
        outputs_.back().height = h;
    }
}
void WaylandCore::handle_output_scale(int32_t f) {
    if (!outputs_.empty()) outputs_.back().scale = f;
}
void WaylandCore::handle_output_name(const std::string& n) {
    if (!outputs_.empty()) outputs_.back().name = n;
}

// --- Backdrop capture (glassmorphism) ---
#ifdef HAS_SCREENCOPY
static void sc_buffer_cb(void* d, zwlr_screencopy_frame_v1*, uint32_t fmt, uint32_t w, uint32_t h,
                         uint32_t stride) {
    static_cast<WaylandCore*>(d)->handle_sc_buffer(fmt, w, h, stride);
}
static void sc_flags_cb(void* d, zwlr_screencopy_frame_v1*, uint32_t flags) {
    static_cast<WaylandCore*>(d)->handle_sc_flags(flags);
}
static void sc_ready_cb(void* d, zwlr_screencopy_frame_v1*, uint32_t, uint32_t, uint32_t) {
    static_cast<WaylandCore*>(d)->handle_sc_ready();
}
static void sc_failed_cb(void* d, zwlr_screencopy_frame_v1*) {
    static_cast<WaylandCore*>(d)->handle_sc_failed();
}
static const zwlr_screencopy_frame_v1_listener sc_frame_listener = {
    .buffer = sc_buffer_cb,
    .flags = sc_flags_cb,
    .ready = sc_ready_cb,
    .failed = sc_failed_cb,
};

void WaylandCore::handle_sc_buffer(uint32_t format, uint32_t w, uint32_t h, uint32_t stride) {
    if (cap_wl_buffer_ || backdrop_failed_) return; // only handle the first offered format
    backdrop_format_ = format;
    backdrop_w_ = static_cast<int>(w);
    backdrop_h_ = static_cast<int>(h);
    backdrop_stride_ = static_cast<int>(stride);
    cap_size_ = static_cast<int>(stride * h);
    if (cap_size_ <= 0) {
        backdrop_failed_ = true;
        return;
    }

    cap_fd_ = create_unlinked_shm_file();
    if (cap_fd_ < 0) {
        backdrop_failed_ = true;
        return;
    }
    if (ftruncate(cap_fd_, cap_size_) < 0) {
        close(cap_fd_);
        cap_fd_ = -1;
        backdrop_failed_ = true;
        return;
    }
    cap_data_ = static_cast<uint8_t*>(
        mmap(nullptr, cap_size_, PROT_READ | PROT_WRITE, MAP_SHARED, cap_fd_, 0));
    if (cap_data_ == MAP_FAILED) {
        cap_data_ = nullptr;
        backdrop_failed_ = true;
        return;
    }

    wl_shm_pool* pool = wl_shm_create_pool(shm_, cap_fd_, cap_size_);
    if (!pool) {
        backdrop_failed_ = true;
        return;
    }
    cap_wl_buffer_ =
        wl_shm_pool_create_buffer(pool, 0, static_cast<int32_t>(w), static_cast<int32_t>(h),
                                  static_cast<int32_t>(stride), format);
    wl_shm_pool_destroy(pool);
    if (!cap_wl_buffer_) {
        backdrop_failed_ = true;
        return;
    }
    zwlr_screencopy_frame_v1_copy(cap_frame_, cap_wl_buffer_);
}

void WaylandCore::handle_sc_flags(uint32_t flags) {
    backdrop_y_invert_ = (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
}
void WaylandCore::handle_sc_ready() { backdrop_ready_ = true; }
void WaylandCore::handle_sc_failed() { backdrop_failed_ = true; }

bool WaylandCore::capture_backdrop() {
    if (!screencopy_manager_ || !shm_) return false;
    wl_output* output = outputs_.empty() ? nullptr : outputs_.front().output;
    if (!output) return false;

    backdrop_ready_ = backdrop_failed_ = false;
    cap_frame_ = zwlr_screencopy_manager_v1_capture_output(screencopy_manager_, 0, output);
    if (!cap_frame_) return false;
    zwlr_screencopy_frame_v1_add_listener(cap_frame_, &sc_frame_listener, this);

    while (!backdrop_ready_ && !backdrop_failed_) {
        if (wl_display_dispatch(display_) < 0) {
            backdrop_failed_ = true;
            break;
        }
    }

    if (backdrop_ready_ && cap_data_ && cap_size_ > 0) {
        backdrop_pixels_.assign(cap_data_, cap_data_ + cap_size_);
        has_backdrop_ = true;
    }

    if (cap_frame_) {
        zwlr_screencopy_frame_v1_destroy(cap_frame_);
        cap_frame_ = nullptr;
    }
    if (cap_wl_buffer_) {
        wl_buffer_destroy(cap_wl_buffer_);
        cap_wl_buffer_ = nullptr;
    }
    if (cap_data_ && cap_size_ > 0) {
        munmap(cap_data_, cap_size_);
        cap_data_ = nullptr;
    }
    if (cap_fd_ >= 0) {
        close(cap_fd_);
        cap_fd_ = -1;
    }
    cap_size_ = 0;

    if (wl_dbg())
        fprintf(stderr, "[wl] backdrop ready=%d fail=%d has=%d fmt=%u %dx%d stride=%d yinv=%d\n",
                backdrop_ready_, backdrop_failed_, has_backdrop_, backdrop_format_, backdrop_w_,
                backdrop_h_, backdrop_stride_, backdrop_y_invert_);
    return has_backdrop_;
}

bool WaylandCore::has_backdrop() const { return has_backdrop_; }
const uint8_t* WaylandCore::backdrop_data() const { return backdrop_pixels_.data(); }
int WaylandCore::backdrop_width() const { return backdrop_w_; }
int WaylandCore::backdrop_height() const { return backdrop_h_; }
int WaylandCore::backdrop_stride() const { return backdrop_stride_; }
uint32_t WaylandCore::backdrop_format() const { return backdrop_format_; }
bool WaylandCore::backdrop_y_invert() const { return backdrop_y_invert_; }
#else
bool WaylandCore::capture_backdrop() { return false; }
bool WaylandCore::has_backdrop() const { return false; }
const uint8_t* WaylandCore::backdrop_data() const { return nullptr; }
int WaylandCore::backdrop_width() const { return 0; }
int WaylandCore::backdrop_height() const { return 0; }
int WaylandCore::backdrop_stride() const { return 0; }
uint32_t WaylandCore::backdrop_format() const { return 0; }
bool WaylandCore::backdrop_y_invert() const { return false; }
#endif

} // namespace waylaunch
