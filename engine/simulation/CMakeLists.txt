# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::simulation — tick scheduling, and later commands, systems, hashing, and snapshots.
#
# Contains no game rules, and no dependency on the platform or the renderer. At M1 it holds
# only the tick accumulator, which is pure integer arithmetic over an elapsed duration.
atlas_add_module(simulation
    SOURCES
        src/tick_accumulator.cpp
    HEADERS
        include/atlas/simulation/tick_accumulator.hpp
    DEPENDS
        core
)

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
