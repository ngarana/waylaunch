cmake_minimum_required(VERSION 3.20)
project(waylaunch VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find required packages
find_package(PkgConfig REQUIRED)

pkg_check_modules(WAYLAND REQUIRED wayland-client)
pkg_check_modules(WAYLAND_CURSOR REQUIRED wayland-cursor)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
pkg_check_modules(CAIRO REQUIRED cairo)
pkg_check_modules(PANGO REQUIRED pangocairo)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
pkg_check_modules(GDK_PIXBUF REQUIRED gdk-pixbuf-2.0)
pkg_check_modules(GTK REQUIRED gtk+-3.0)
pkg_check_modules(LIBRSVG REQUIRED librsvg-2.0)

# Generate Wayland protocol sources
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

set(PROTOCOL_BUILD_DIR ${CMAKE_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${PROTOCOL_BUILD_DIR})

# XDG Shell protocol
pkg_check_modules(WAYLAND_PROTOCOLS wayland-protocols)
find_file(XDG_SHELL_XML xdg-shell.xml
    PATHS ${WAYLAND_PROTOCOLS_DATADIR}/wayland-protocols/stable/xdg-shell
    NO_DEFAULT_PATH)
if(NOT XDG_SHELL_XML)
    set(XDG_SHELL_XML "${WAYLAND_PROTOCOLS_PREFIX}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml")
endif()

add_custom_command(
    OUTPUT ${PROTOCOL_BUILD_DIR}/xdg-shell-protocol.c ${PROTOCOL_BUILD_DIR}/xdg-shell-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} private-code ${XDG_SHELL_XML} ${PROTOCOL_BUILD_DIR}/xdg-shell-protocol.c
    COMMAND ${WAYLAND_SCANNER} client-header ${XDG_SHELL_XML} ${PROTOCOL_BUILD_DIR}/xdg-shell-client-protocol.h
    DEPENDS ${XDG_SHELL_XML}
)

# wlr-layer-shell protocol (unstable)
set(LAYER_SHELL_XML ${CMAKE_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml)
if(EXISTS ${LAYER_SHELL_XML})
    add_custom_command(
        OUTPUT ${PROTOCOL_BUILD_DIR}/wlr-layer-shell-protocol.c ${PROTOCOL_BUILD_DIR}/wlr-layer-shell-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} private-code ${LAYER_SHELL_XML} ${PROTOCOL_BUILD_DIR}/wlr-layer-shell-protocol.c
        COMMAND ${WAYLAND_SCANNER} client-header ${LAYER_SHELL_XML} ${PROTOCOL_BUILD_DIR}/wlr-layer-shell-client-protocol.h
        DEPENDS ${LAYER_SHELL_XML}
    )
    set(HAS_LAYER_SHELL TRUE)
else()
    message(WARNING "wlr-layer-shell protocol XML not found, layer-shell support disabled")
    set(HAS_LAYER_SHELL FALSE)
endif()

# wlr-foreign-toplevel protocol
set(FOREIGN_TOPLEVEL_XML ${CMAKE_SOURCE_DIR}/protocols/wlr-foreign-toplevel-management-unstable-v1.xml)
if(EXISTS ${FOREIGN_TOPLEVEL_XML})
    add_custom_command(
        OUTPUT ${PROTOCOL_BUILD_DIR}/wlr-foreign-toplevel-protocol.c ${PROTOCOL_BUILD_DIR}/wlr-foreign-toplevel-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} private-code ${FOREIGN_TOPLEVEL_XML} ${PROTOCOL_BUILD_DIR}/wlr-foreign-toplevel-protocol.c
        COMMAND ${WAYLAND_SCANNER} client-header ${FOREIGN_TOPLEVEL_XML} ${PROTOCOL_BUILD_DIR}/wlr-foreign-toplevel-client-protocol.h
        DEPENDS ${FOREIGN_TOPLEVEL_XML}
    )
    set(HAS_FOREIGN_TOPLEVEL TRUE)
else()
    message(WARNING "wlr-foreign-toplevel protocol XML not found, foreign-toplevel support disabled")
    set(HAS_FOREIGN_TOPLEVEL FALSE)
endif()

# wlr-screencopy protocol (for client-side backdrop blur / glassmorphism)
set(SCREENCOPY_XML ${CMAKE_SOURCE_DIR}/protocols/wlr-screencopy-unstable-v1.xml)
if(EXISTS ${SCREENCOPY_XML})
    add_custom_command(
        OUTPUT ${PROTOCOL_BUILD_DIR}/wlr-screencopy-protocol.c ${PROTOCOL_BUILD_DIR}/wlr-screencopy-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} private-code ${SCREENCOPY_XML} ${PROTOCOL_BUILD_DIR}/wlr-screencopy-protocol.c
        COMMAND ${WAYLAND_SCANNER} client-header ${SCREENCOPY_XML} ${PROTOCOL_BUILD_DIR}/wlr-screencopy-client-protocol.h
        DEPENDS ${SCREENCOPY_XML}
    )
    set(HAS_SCREENCOPY TRUE)
else()
    set(HAS_SCREENCOPY FALSE)
endif()

# Collect generated protocol sources
set(PROTOCOL_SOURCES
    ${PROTOCOL_BUILD_DIR}/xdg-shell-protocol.c
)
if(HAS_LAYER_SHELL)
    list(APPEND PROTOCOL_SOURCES ${PROTOCOL_BUILD_DIR}/wlr-layer-shell-protocol.c)
endif()
if(HAS_SCREENCOPY)
    list(APPEND PROTOCOL_SOURCES ${PROTOCOL_BUILD_DIR}/wlr-screencopy-protocol.c)
endif()
if(HAS_FOREIGN_TOPLEVEL)
    list(APPEND PROTOCOL_SOURCES ${PROTOCOL_BUILD_DIR}/wlr-foreign-toplevel-protocol.c)
endif()

# Main sources
set(SOURCES
    src/main.cpp
    src/core/wayland_core.cpp
    src/ui/renderer.cpp
    src/ui/launcher_ui.cpp
    src/search/subprocess.cpp
    src/modes/app_launcher.cpp
    src/modes/calculator.cpp
    src/modes/clipboard.cpp
    src/config/config.cpp
    ${PROTOCOL_SOURCES}
)

# Create executable
add_executable(waylaunch ${SOURCES})

target_include_directories(waylaunch PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PROTOCOL_BUILD_DIR}
    ${WAYLAND_INCLUDE_DIRS}
    ${WAYLAND_CURSOR_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
    ${PANGO_INCLUDE_DIRS}
    ${FONTCONFIG_INCLUDE_DIRS}
    ${GDK_PIXBUF_INCLUDE_DIRS}
    ${GTK_INCLUDE_DIRS}
    ${LIBRSVG_INCLUDE_DIRS}
)

target_link_libraries(waylaunch PRIVATE
    ${WAYLAND_LIBRARIES}
    ${WAYLAND_CURSOR_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
    ${CAIRO_LIBRARIES}
    ${PANGO_LIBRARIES}
    ${FONTCONFIG_LIBRARIES}
    ${GDK_PIXBUF_LIBRARIES}
    ${GTK_LIBRARIES}
    ${LIBRSVG_LIBRARIES}
    rt   # for shm_open
    m    # math
)

target_compile_options(waylaunch PRIVATE
    ${WAYLAND_CFLAGS}
    ${WAYLAND_CURSOR_CFLAGS}
    ${XKBCOMMON_CFLAGS}
    ${CAIRO_CFLAGS}
    ${PANGO_CFLAGS}
    ${FONTCONFIG_CFLAGS}
    ${GDK_PIXBUF_CFLAGS}
    ${GTK_CFLAGS}
    ${LIBRSVG_CFLAGS}
    -Wall -Wextra -Wpedantic
)

# Add compile definitions for protocol support
if(HAS_LAYER_SHELL)
    target_compile_definitions(waylaunch PRIVATE HAS_LAYER_SHELL)
endif()
if(HAS_FOREIGN_TOPLEVEL)
    target_compile_definitions(waylaunch PRIVATE HAS_FOREIGN_TOPLEVEL)
endif()
if(HAS_SCREENCOPY)
    target_compile_definitions(waylaunch PRIVATE HAS_SCREENCOPY)
endif()

# Install
install(TARGETS waylaunch DESTINATION bin)
install(FILES config/waylaunch.toml DESTINATION ${CMAKE_INSTALL_PREFIX}/share/waylaunch)
install(FILES doc/waylaunch.1 DESTINATION ${CMAKE_INSTALL_PREFIX}/share/man/man1)

# toml++ (system package)
find_package(tomlplusplus 3 CONFIG REQUIRED)
target_link_libraries(waylaunch PRIVATE tomlplusplus::tomlplusplus)
