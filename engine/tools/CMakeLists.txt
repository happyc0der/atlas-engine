# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::tools — the internal engineering overlay.
#
# Nothing in the engine depends on this; it depends on the engine. The immediate-mode library
# is private, and its input is driven from Atlas's own event types rather than through its
# platform backend, which would have required window-system types inside this module.
find_package(imgui CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)

atlas_add_module(tools
    SOURCES
        src/debug_ui.cpp
    HEADERS
        include/atlas/tools/debug_ui.hpp
    DEPENDS
        core
        platform
        rhi
        rhi_internal
    PRIVATE_DEPS
        imgui::imgui
        SDL3::SDL3-static
)
