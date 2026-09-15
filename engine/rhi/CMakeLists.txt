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
        src/textures.cpp
        src/types.cpp
        src/sdl_gpu_conv.cpp
        src/sdl_gpu_access.cpp
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

target_include_directories(atlas_rhi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/internal/include)

# The native graphics handles, for the debug overlay and for nothing else. A separate target
# rather than a public header, so the one consumer is named in the build system.
add_library(atlas_rhi_internal INTERFACE)
add_library(atlas::rhi_internal ALIAS atlas_rhi_internal)
target_include_directories(atlas_rhi_internal
    INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/internal/include>)
target_link_libraries(atlas_rhi_internal INTERFACE atlas::rhi)

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
