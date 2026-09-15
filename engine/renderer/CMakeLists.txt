# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::renderer — drawing built on the render hardware interface.
#
# Reads; never writes. It consumes camera transforms and quad descriptions and turns them
# into draw calls, and it mutates no simulation or scene state.
#
# The generated shader manifest is on the private include path: resource counts come from
# reflection over the compiled shaders, so they cannot disagree with them. See ADR-0006.
atlas_add_module(renderer
    SOURCES
        src/quad_batch.cpp
        src/shader_loader.cpp
    HEADERS
        include/atlas/renderer/quad_batch.hpp
    DEPENDS
        core
        math
        rhi
)

target_include_directories(atlas_renderer
    PRIVATE "${PROJECT_SOURCE_DIR}/assets/cooked/shaders")

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
