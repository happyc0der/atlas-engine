# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::rhi — the render hardware interface.
#
# Everything the graphics library defines stays inside this module's sources. The public
# headers name handles, descriptors and enumerations of Atlas's own, so that replacing
# SDL_GPU with a native backend rewrites this directory and nothing above it.
# See docs/adr/0002-rendering-backend.md.
find_package(SDL3 CONFIG REQUIRED)

atlas_add_module(rhi
    SOURCES
        src/device.cpp
        src/resources.cpp
        src/types.cpp
        src/sdl_gpu_conv.cpp
    HEADERS
        include/atlas/rhi/descs.hpp
        include/atlas/rhi/device.hpp
        include/atlas/rhi/handles.hpp
        include/atlas/rhi/types.hpp
    DEPENDS
        core
        platform
        platform_internal
    PRIVATE_DEPS
        SDL3::SDL3-static
)

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
