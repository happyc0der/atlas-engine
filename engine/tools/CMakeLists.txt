# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::tools — the internal engineering overlay.
#
# Nothing in the engine depends on this; it depends on the engine. The immediate-mode library
# is private, and its input is driven from Atlas's own event types rather than through its
# platform backend, which would have required window-system types inside this module.
find_package(imgui CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
atlas_define_sdl3_target()

atlas_add_module(tools
    SOURCES
        src/debug_ui.cpp
        src/imgui_keymap.cpp
        src/panels.cpp
    HEADERS
        include/atlas/tools/debug_ui.hpp
        include/atlas/tools/panels.hpp
        include/atlas/tools/text_keys.hpp
    DEPENDS
        core
        assets
        edit
        simulation
        platform
        rhi
        rhi_internal
        math
        scene
        text
    PRIVATE_DEPS
        imgui::imgui
        atlas::sdl3
)

add_subdirectory(tests)
