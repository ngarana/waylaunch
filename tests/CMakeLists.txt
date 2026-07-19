cmake_minimum_required(VERSION 3.20)
project(waylaunch_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
pkg_check_modules(CAIRO REQUIRED cairo)
pkg_check_modules(PANGO REQUIRED pangocairo)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)
pkg_check_modules(GDK_PIXBUF REQUIRED gdk-pixbuf-2.0)
pkg_check_modules(GTK REQUIRED gtk+-3.0)
pkg_check_modules(LIBRSVG REQUIRED librsvg-2.0)

# Find toml++ and nlohmann-json
find_package(tomlplusplus 3 CONFIG REQUIRED)
find_package(nlohmann_json 3 CONFIG REQUIRED)

# Include paths
set(WAYLAUNCH_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/../include)

# Collect test sources
set(TEST_SOURCES
    test_main.cpp
)

# Create test executable
add_executable(waylaunch_tests ${TEST_SOURCES})

target_include_directories(waylaunch_tests PRIVATE
    ${WAYLAUNCH_INCLUDE_DIR}
    ${WAYLAND_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
    ${PANGO_INCLUDE_DIRS}
    ${FONTCONFIG_INCLUDE_DIRS}
)

target_link_libraries(waylaunch_tests PRIVATE
    waylaunch_core
    ${WAYLAND_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
    ${CAIRO_LIBRARIES}
    ${PANGO_LIBRARIES}
    ${FONTCONFIG_LIBRARIES}
    tomlplusplus::tomlplusplus
    nlohmann_json::nlohmann_json
    rt
    m
)

target_compile_options(waylaunch_tests PRIVATE
    ${WAYLAND_CFLAGS}
    ${XKBCOMMON_CFLAGS}
    ${CAIRO_CFLAGS}
    ${PANGO_CFLAGS}
    ${FONTCONFIG_CFLAGS}
    -Wall -Wextra -Wpedantic
)

# Build the library from sources that don't depend on Wayland window
add_library(waylaunch_core STATIC
    ${CMAKE_SOURCE_DIR}/../src/search/search_manager.cpp
    ${CMAKE_SOURCE_DIR}/../src/search/subprocess.cpp
    ${CMAKE_SOURCE_DIR}/../src/ui/renderer.cpp
    ${CMAKE_SOURCE_DIR}/../src/config/config.cpp
)

target_include_directories(waylaunch_core PUBLIC
    ${WAYLAUNCH_INCLUDE_DIR}
    ${WAYLAND_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
    ${CAIRO_INCLUDE_DIRS}
    ${PANGO_INCLUDE_DIRS}
    ${FONTCONFIG_INCLUDE_DIRS}
    ${GDK_PIXBUF_INCLUDE_DIRS}
    ${GTK_INCLUDE_DIRS}
    ${LIBRSVG_INCLUDE_DIRS}
)

target_link_libraries(waylaunch_core PUBLIC
    ${WAYLAND_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
    ${CAIRO_LIBRARIES}
    ${PANGO_LIBRARIES}
    ${FONTCONFIG_LIBRARIES}
    ${GDK_PIXBUF_LIBRARIES}
    ${GTK_LIBRARIES}
    ${LIBRSVG_LIBRARIES}
    tomlplusplus::tomlplusplus
    nlohmann_json::nlohmann_json
    rt
    m
)

target_compile_options(waylaunch_core PRIVATE
    ${WAYLAND_CFLAGS}
    ${XKBCOMMON_CFLAGS}
    ${CAIRO_CFLAGS}
    ${PANGO_CFLAGS}
    ${FONTCONFIG_CFLAGS}
    -Wall -Wextra -Wpedantic
)
