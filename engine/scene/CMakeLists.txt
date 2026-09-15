# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::scene — presentation entities, their transforms, and the tree they form.
#
# Not the grand-strategy database. Simulation data will live in structure-of-arrays tables
# chosen from measured query patterns; see docs/adr/0004-scene-ecs-vs-simulation-storage.md.
#
# The entity library and the JSON reader are both private: every use of either is confined to
# this module's sources, which is what makes the pins moveable.
find_package(EnTT CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

atlas_add_module(scene
    SOURCES
        src/scene.cpp
        src/serialization.cpp
    HEADERS
        include/atlas/scene/components.hpp
        include/atlas/scene/scene.hpp
        include/atlas/scene/serialization.hpp
    DEPENDS
        assets
        core
        math
        rhi
    PRIVATE_DEPS
        EnTT::EnTT
        nlohmann_json::nlohmann_json
)

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
