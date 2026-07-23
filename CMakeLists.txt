cmake_minimum_required(VERSION 3.20)
project(waylaunch VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find required packages
find_package(PkgConfig REQUIRED)

pkg_check_modules(WAYLAND REQUIRED wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
pkg_check_modules(CAIRO REQUIRED cairo)
pkg_check_modules(PANGO REQUIRED pangocairo)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
pkg_check_modules(GDK_PIXBUF REQUIRED gdk-pixbuf-2.0)
pkg_check_modules(GTK REQUIRED gtk+-3.0)

# Content-search index (waylaunchd + read-only launcher queries).
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
pkg_check_modules(ZSTD REQUIRED libzstd)   # compressed doc store (NFR5)
pkg_check_modules(LIBMAGIC libmagic)   # optional: MIME sniffing (falls back to ext)
find_package(Threads REQUIRED)
find_package(tomlplusplus 3 CONFIG REQUIRED)

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

# wlr-foreign-toplevel-management protocol (for Command+Tab app switcher)
set(FOREIGN_TOPLEVEL_XML ${CMAKE_SOURCE_DIR}/protocols/wlr-foreign-toplevel-management-unstable-v1.xml)
if(EXISTS ${FOREIGN_TOPLEVEL_XML})
    add_custom_command(
        OUTPUT ${PROTOCOL_BUILD_DIR}/wlr-foreign-toplevel-management-protocol.c ${PROTOCOL_BUILD_DIR}/wlr-foreign-toplevel-management-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} private-code ${FOREIGN_TOPLEVEL_XML} ${PROTOCOL_BUILD_DIR}/wlr-foreign-toplevel-management-protocol.c
        COMMAND ${WAYLAND_SCANNER} client-header ${FOREIGN_TOPLEVEL_XML} ${PROTOCOL_BUILD_DIR}/wlr-foreign-toplevel-management-client-protocol.h
        DEPENDS ${FOREIGN_TOPLEVEL_XML}
    )
    set(HAS_FOREIGN_TOPLEVEL TRUE)
else()
    set(HAS_FOREIGN_TOPLEVEL FALSE)
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
    list(APPEND PROTOCOL_SOURCES ${PROTOCOL_BUILD_DIR}/wlr-foreign-toplevel-management-protocol.c)
endif()

# ---------------------------------------------------------------------------
# waylaunch_content — shared content-search library (index store + extraction +
# discovery). Linked read-only into the launcher and read-write into waylaunchd.
# ---------------------------------------------------------------------------
add_library(waylaunch_content STATIC
    src/content/store.cpp
    src/content/extractor.cpp
    src/content/config.cpp
    src/content/indexer.cpp
    src/content/fs_watcher.cpp
    src/content/control.cpp
    src/search/subprocess.cpp
)
target_include_directories(waylaunch_content PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${SQLITE3_INCLUDE_DIRS}
    ${ZSTD_INCLUDE_DIRS}
)
target_link_libraries(waylaunch_content PUBLIC ${SQLITE3_LIBRARIES} ${ZSTD_LIBRARIES} tomlplusplus::tomlplusplus Threads::Threads)
target_compile_options(waylaunch_content PRIVATE
    ${SQLITE3_CFLAGS} -Wall -Wextra)
if(LIBMAGIC_FOUND)
    target_compile_definitions(waylaunch_content PUBLIC HAVE_LIBMAGIC)
    target_include_directories(waylaunch_content PRIVATE ${LIBMAGIC_INCLUDE_DIRS})
    # libmagic's .pc sometimes omits -lmagic; link it explicitly.
    if(LIBMAGIC_LIBRARIES)
        target_link_libraries(waylaunch_content PUBLIC ${LIBMAGIC_LIBRARIES})
    else()
        target_link_libraries(waylaunch_content PUBLIC magic)
    endif()
endif()

# Main sources
set(SOURCES
    src/main.cpp
    src/core/wayland_core.cpp
    src/ui/renderer.cpp
    src/ui/launcher_ui.cpp
    src/modes/app_launcher.cpp
    src/modes/calculator.cpp
    src/modes/clipboard.cpp
    src/config/config.cpp
    src/switcher/wlr_toplevel_backend.cpp
    src/switcher/app_switcher_manager.cpp
    src/switcher/switcher_state_machine.cpp
    src/switcher/switcher_input_controller.cpp
    src/switcher/switcher_renderer.cpp
    src/power/power_action_backend.cpp
    src/power/power_manager.cpp
    src/power/power_state_machine.cpp
    src/power/power_input_controller.cpp
    src/power/power_renderer.cpp
    src/power/confirm_dialog_renderer.cpp
    src/power/power_glyphs.cpp
    ${PROTOCOL_SOURCES}
)

# Create executable
add_executable(waylaunch ${SOURCES})

target_include_directories(waylaunch PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PROTOCOL_BUILD_DIR}
    ${WAYLAND_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
    ${PANGO_INCLUDE_DIRS}
    ${FONTCONFIG_INCLUDE_DIRS}
    ${GDK_PIXBUF_INCLUDE_DIRS}
    ${GTK_INCLUDE_DIRS}
)

target_link_libraries(waylaunch PRIVATE
    waylaunch_content   # read-only content-index queries
    ${WAYLAND_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
    ${CAIRO_LIBRARIES}
    ${PANGO_LIBRARIES}
    ${FONTCONFIG_LIBRARIES}
    ${GDK_PIXBUF_LIBRARIES}
    ${GTK_LIBRARIES}
    rt   # for shm_open
    m    # math
)

target_compile_options(waylaunch PRIVATE
    ${WAYLAND_CFLAGS}
    ${XKBCOMMON_CFLAGS}
    ${CAIRO_CFLAGS}
    ${PANGO_CFLAGS}
    ${FONTCONFIG_CFLAGS}
    ${GDK_PIXBUF_CFLAGS}
    ${GTK_CFLAGS}
    -Wall -Wextra -Wpedantic
)

# Add compile definitions for protocol support
if(HAS_LAYER_SHELL)
    target_compile_definitions(waylaunch PRIVATE HAS_LAYER_SHELL)
endif()
if(HAS_SCREENCOPY)
    target_compile_definitions(waylaunch PRIVATE HAS_SCREENCOPY)
endif()
if(HAS_FOREIGN_TOPLEVEL)
    target_compile_definitions(waylaunch PRIVATE HAS_FOREIGN_TOPLEVEL)
endif()

# waylaunchd (indexing daemon) + waylaunchctl (control CLI)
add_executable(waylaunchd src/content/waylaunchd_main.cpp)
target_link_libraries(waylaunchd PRIVATE waylaunch_content)
target_compile_options(waylaunchd PRIVATE -Wall -Wextra)

add_executable(waylaunchctl src/content/waylaunchctl_main.cpp)
target_link_libraries(waylaunchctl PRIVATE waylaunch_content)
target_compile_options(waylaunchctl PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------------
# Tests (ctest): content store / extractor / freshness. Enable with -DBUILD_TESTING=ON.
# ---------------------------------------------------------------------------
option(BUILD_TESTING "Build the content-search test suite" ON)
if(BUILD_TESTING)
    enable_testing()
    foreach(t content_store_test content_extractor_test content_indexer_test
              content_freshness_test)
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE waylaunch_content)
        target_compile_options(${t} PRIVATE -Wall -Wextra)
        add_test(NAME ${t} COMMAND ${t})
    endforeach()

    add_executable(app_switcher_test
        tests/app_switcher_test.cpp
        src/switcher/wlr_toplevel_backend.cpp
        src/switcher/app_switcher_manager.cpp
        src/switcher/switcher_state_machine.cpp
        src/switcher/switcher_input_controller.cpp
        src/switcher/switcher_renderer.cpp
        src/ui/renderer.cpp
        ${PROTOCOL_SOURCES}
    )
    target_include_directories(app_switcher_test PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${PROTOCOL_BUILD_DIR}
        ${WAYLAND_INCLUDE_DIRS}
        ${XKBCOMMON_INCLUDE_DIRS}
        ${CAIRO_INCLUDE_DIRS}
        ${PANGO_INCLUDE_DIRS}
        ${FONTCONFIG_INCLUDE_DIRS}
        ${GDK_PIXBUF_INCLUDE_DIRS}
        ${GTK_INCLUDE_DIRS}
    )
    target_link_libraries(app_switcher_test PRIVATE
        ${WAYLAND_LIBRARIES}
        ${XKBCOMMON_LIBRARIES}
        ${CAIRO_LIBRARIES}
        ${PANGO_LIBRARIES}
        ${FONTCONFIG_LIBRARIES}
        ${GDK_PIXBUF_LIBRARIES}
        ${GTK_LIBRARIES}
    )
    if(HAS_FOREIGN_TOPLEVEL)
        target_compile_definitions(app_switcher_test PRIVATE HAS_FOREIGN_TOPLEVEL)
    endif()
    # assert-based — keep the checks alive under Release-family builds (NDEBUG).
    target_compile_options(app_switcher_test PRIVATE -UNDEBUG)
    add_test(NAME app_switcher_test COMMAND app_switcher_test)
    set_tests_properties(content_indexer_test PROPERTIES TIMEOUT 60)
    set_tests_properties(content_freshness_test PROPERTIES TIMEOUT 60)

    # Power overlay unit tests — header-only includable (no Wayland, no cairo):
    # they instantiate PowerManager with a stub backend, mirroring the switcher
    # tests. waylaunch_content supplies Subprocess + toml++ include dirs.
    add_executable(power_state_machine_test tests/power_state_machine_test.cpp
        src/power/power_state_machine.cpp)
    add_executable(confirm_dialog_test tests/confirm_dialog_test.cpp)
    add_executable(power_action_backend_test tests/power_action_backend_test.cpp
        src/power/power_action_backend.cpp)
    add_executable(power_manager_test tests/power_manager_test.cpp
        src/power/power_manager.cpp
        src/power/power_input_controller.cpp
        src/power/power_state_machine.cpp)
    foreach(t power_state_machine_test confirm_dialog_test
              power_action_backend_test power_manager_test)
        target_link_libraries(${t} PRIVATE waylaunch_content)
        target_include_directories(${t} PRIVATE ${XKBCOMMON_INCLUDE_DIRS})
        # -UNDEBUG: these are assert-based; Release-family builds define NDEBUG
        # which would silently turn every check into a no-op.
        target_compile_options(${t} PRIVATE -Wall -Wextra -UNDEBUG)
        add_test(NAME ${t} COMMAND ${t})
    endforeach()

    # Performance harness (NFR1/NFR5) — built but not a pass/fail ctest; run
    # manually with a doc count, e.g. ./content_bench 1000000.
    add_executable(content_bench tests/content_bench.cpp)
    target_link_libraries(content_bench PRIVATE waylaunch_content)
    target_compile_options(content_bench PRIVATE -Wall -Wextra -O2)
endif()

# Install
include(GNUInstallDirs)
install(TARGETS waylaunch waylaunchd waylaunchctl DESTINATION bin)
# Generate the systemd unit with ExecStart pointing at the real install prefix
# (CMAKE_INSTALL_FULL_BINDIR) so `cmake --install` to /usr or /usr/local both work.
configure_file(dist/waylaunchd.service.in
               ${CMAKE_BINARY_DIR}/waylaunchd.service @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/waylaunchd.service
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user)
install(FILES config/waylaunch.toml DESTINATION ${CMAKE_INSTALL_PREFIX}/share/waylaunch)
install(FILES doc/waylaunch.1 DESTINATION ${CMAKE_INSTALL_PREFIX}/share/man/man1)

# toml++ (system package)
target_link_libraries(waylaunch PRIVATE tomlplusplus::tomlplusplus)
