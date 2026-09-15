# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::platform — the window system, the event queue, input, and the lifetime of SDL.
#
# SDL3 is a PRIVATE dependency. For a static library that propagates as $<LINK_ONLY:>, so
# SDL's include directories never reach a consumer: a public header that included an SDL
# header would fail to compile in the sandbox. That is the enforcement; the boundary script
# is the backstop.
find_package(SDL3 CONFIG REQUIRED)
atlas_assert_from_vcpkg(SDL3 "${SDL3_DIR}")

atlas_add_module(platform
    SOURCES
        src/platform.cpp
        src/window.cpp
        src/input.cpp
        src/event.cpp
        src/key.cpp
        src/sdl_keymap.cpp
        src/sdl_error.cpp
    HEADERS
        include/atlas/platform/event.hpp
        include/atlas/platform/input.hpp
        include/atlas/platform/key.hpp
        include/atlas/platform/platform.hpp
        include/atlas/platform/types.hpp
        include/atlas/platform/window.hpp
    DEPENDS
        core
    PRIVATE_DEPS
        SDL3::SDL3-static
)

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
