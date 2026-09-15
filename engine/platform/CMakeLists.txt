# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::platform — the window system, the event queue, input, and the lifetime of SDL.
#
# SDL3 is a PRIVATE dependency. For a static library that propagates as $<LINK_ONLY:>, so
# SDL's include directories never reach a consumer: a public header that included an SDL
# header would fail to compile in the sandbox. That is the enforcement; the boundary script
# is the backstop.
find_package(SDL3 CONFIG REQUIRED)
atlas_define_sdl3_target()
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
        src/sdl_access.cpp
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
        atlas::sdl3
)

target_include_directories(atlas_platform PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/internal/include)

# The native window handle, for the renderer and for nothing else. A separate target rather
# than a public header, so that the one consumer is named in the build system and any other
# use fails to configure.
add_library(atlas_platform_internal INTERFACE)
add_library(atlas::platform_internal ALIAS atlas_platform_internal)
target_include_directories(atlas_platform_internal
    INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/internal/include>)
target_link_libraries(atlas_platform_internal INTERFACE atlas::platform)

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
