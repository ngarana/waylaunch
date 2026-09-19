# Post-process a wayland-scanner client header for C++ inclusion.
#
# wayland-scanner emits get_layer_surface's argument literally named
# `namespace`, which is a C++ keyword and breaks any C++ translation unit
# including the header. Rename the identifier (whole-word).
#
# A bare `sed s/\bnamespace\b/.../` in the custom command is NOT portable:
# Unix Makefiles run recipes through /bin/sh, which strips the backslashes
# before sed ever sees them (qypr gets away with it because it builds with
# Ninja, which execs directly). Doing the rewrite here keeps it correct
# under every generator.
#
# Usage:
#   cmake -DHEADER=<path-to-client-protocol.h> -P fix_scanner_keywords.cmake
if(NOT DEFINED HEADER)
    message(FATAL_ERROR "fix_scanner_keywords.cmake requires -DHEADER=<file>")
endif()
file(READ "${HEADER}" _content)
# NOTE: no \b — CMake's regex engine has no word-boundary anchor. The explicit
# class covers every occurrence (`*namespace)`, `, namespace)`, "a namespace").
string(REGEX REPLACE "([^A-Za-z0-9_])namespace([^A-Za-z0-9_])" "\\1wl_namespace\\2"
                     _content "${_content}")
file(WRITE "${HEADER}" "${_content}")
