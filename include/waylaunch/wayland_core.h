#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <xkbcommon/xkbcommon.h>

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_shm;
struct wl_seat;
struct wl_keyboard;
struct wl_pointer;
struct wl_output;
struct wl_surface;
struct wl_buffer;
struct wl_shm_pool;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;

#ifdef HAS_LAYER_SHELL
struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;
#endif

#ifdef HAS_FOREIGN_TOPLEVEL
struct zwlr_foreign_toplevel_manager_v1;
struct zwlr_foreign_toplevel_handle_v1;
#endif

#ifdef HAS_SCREENCOPY
struct zwlr_screencopy_manager_v1;
struct zwlr_screencopy_frame_v1;
#endif

namespace waylaunch {

struct Buffer {
    int fd = -1;
    uint8_t* data = nullptr;
    wl_buffer* wl_buf = nullptr;
    wl_shm_pool* pool = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    int size = 0;
    bool busy = false;

    Buffer() = default;
    ~Buffer();
    void destroy();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
};

struct KeyboardState {
    xkb_context* xkb_ctx = nullptr;
    xkb_keymap* keymap = nullptr;
    xkb_state* state = nullptr;
    int repeat_rate = 0;
    int repeat_delay = 0;
    KeyboardState();
    ~KeyboardState();
};

struct OutputInfo {
    wl_output* output = nullptr;
    int32_t width = 0;
    int32_t height = 0;
    int32_t scale = 1;
    std::string name;
};

using KeyHandler = std::function<void(uint32_t keysym, uint32_t utf32, bool pressed)>;
using ModifiersHandler = std::function<void(uint32_t mods)>;
using MouseHandler = std::function<void(double x, double y, uint32_t button, bool pressed)>;
using AxisHandler = std::function<void(double x, double y, int32_t axis, double value)>;
using CloseHandler = std::function<void()>;
using RedrawHandler = std::function<void()>;

class WaylandCore {
public:
    WaylandCore();
    ~WaylandCore();

    bool init();
    void run();
    void set_running(bool v);   // begin/stop the external dispatch loop
    void quit();

    Buffer* acquire_buffer();
    void submit_buffer(Buffer* buf, int x = 0, int y = 0);

    void set_key_handler(KeyHandler handler);
    void set_modifiers_handler(ModifiersHandler handler);
    void set_mouse_handler(MouseHandler handler);
    void set_axis_handler(AxisHandler handler);
    void set_close_handler(CloseHandler handler);
    void set_redraw_handler(RedrawHandler handler);

    OutputInfo& primary_output();
    int32_t primary_scale() const;
    int32_t output_width() const;
    int32_t output_height() const;
    int32_t surface_width() const;   // current surface/buffer width (may differ from output)
    int32_t surface_height() const;

    wl_display* display() const;
    wl_surface* surface() const;
    bool is_running() const;
    bool is_configured() const;   // true once the (layer/xdg) surface has been configured

    // Client-side backdrop capture for glassmorphism: grabs the primary output
    // into an SHM buffer BEFORE the overlay is mapped, so we can blur it ourselves.
    bool capture_backdrop();
    void set_want_backdrop(bool v);   // if false, skip the (screencopy) capture entirely
    bool has_backdrop() const;
    const uint8_t* backdrop_data() const;
    int backdrop_width() const;
    int backdrop_height() const;
    int backdrop_stride() const;
    uint32_t backdrop_format() const;
    bool backdrop_y_invert() const;

    void handle_keymap(uint32_t format, int32_t fd, uint32_t size);
    void handle_key(uint32_t serial, uint32_t time, uint32_t key, uint32_t state);
    void handle_modifiers(uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group);
    void handle_output_geometry(int32_t x, int32_t y, int32_t w, int32_t h, int32_t transform, int32_t factor);
    void handle_output_mode(uint32_t flags, int32_t width, int32_t height, int32_t refresh);
    void handle_output_scale(int32_t factor);
    void handle_output_name(const std::string& name);
    void handle_xdg_surface_configure(uint32_t serial);
    void handle_xdg_toplevel_configure(int32_t width, int32_t height);
    void handle_xdg_toplevel_close();
    void handle_buffer_release(wl_buffer* buf);

    // Public for C trampolines
    wl_keyboard* keyboard_ = nullptr;
    wl_pointer* pointer_ = nullptr;
    double pointer_x_ = 0, pointer_y_ = 0;
    KeyboardState kbd_;
    wl_compositor* compositor_ = nullptr;
    wl_shm* shm_ = nullptr;
    wl_seat* seat_ = nullptr;
    xdg_wm_base* xdg_wm_base_ = nullptr;
    std::vector<OutputInfo> outputs_;
    std::vector<std::unique_ptr<Buffer>> buffers_;
    KeyHandler key_handler_;
    ModifiersHandler modifiers_handler_;
    MouseHandler mouse_handler_;
    AxisHandler axis_handler_;
    CloseHandler close_handler_;
    RedrawHandler redraw_handler_;

#ifdef HAS_LAYER_SHELL
    zwlr_layer_shell_v1* layer_shell_ = nullptr;
    zwlr_layer_surface_v1* layer_surface_ = nullptr;
#endif

#ifdef HAS_SCREENCOPY
    zwlr_screencopy_manager_v1* screencopy_manager_ = nullptr;
    // Backdrop capture state (written by frame-listener trampolines).
    std::vector<uint8_t> backdrop_pixels_;
    int backdrop_w_ = 0, backdrop_h_ = 0, backdrop_stride_ = 0;
    uint32_t backdrop_format_ = 0;
    bool backdrop_y_invert_ = false;
    bool backdrop_ready_ = false;
    bool backdrop_failed_ = false;
    bool has_backdrop_ = false;
    // Temporary SHM buffer the compositor copies the frame into.
    int cap_fd_ = -1;
    uint8_t* cap_data_ = nullptr;
    int cap_size_ = 0;
    wl_buffer* cap_wl_buffer_ = nullptr;
    zwlr_screencopy_frame_v1* cap_frame_ = nullptr;
    void handle_sc_buffer(uint32_t format, uint32_t w, uint32_t h, uint32_t stride);
    void handle_sc_flags(uint32_t flags);
    void handle_sc_ready();
    void handle_sc_failed();
#endif

private:
    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_surface* surface_ = nullptr;
    xdg_surface* xdg_surface_ = nullptr;
    xdg_toplevel* toplevel_ = nullptr;

    bool running_ = false;
    bool want_backdrop_ = true;
    int32_t pending_width_ = 0;
    int32_t pending_height_ = 0;
    bool configured_ = false;

    // Key repeat state
    uint32_t repeat_keysym_ = 0;
    uint32_t repeat_utf32_ = 0;
    uint32_t repeat_time_ = 0;
    bool repeat_active_ = false;
};

} // namespace waylaunch
