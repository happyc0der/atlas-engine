# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::simulation — tick scheduling, and later commands, systems, hashing, and snapshots.
#
# Contains no game rules, and no dependency on the platform or the renderer. At M1 it holds
# only the tick accumulator, which is pure integer arithmetic over an elapsed duration.
atlas_add_module(simulation
    SOURCES
        src/tick_accumulator.cpp
        src/save_stream.cpp
        src/command.cpp
        src/kernel.cpp
        src/replay.cpp
        src/save.cpp
        src/schedule.cpp
        src/world.cpp
    HEADERS
        include/atlas/simulation/tick_accumulator.hpp
        include/atlas/simulation/save_stream.hpp
        include/atlas/simulation/command.hpp
        include/atlas/simulation/kernel.hpp
        include/atlas/simulation/rng.hpp
        include/atlas/simulation/schedule.hpp
        include/atlas/simulation/replay.hpp
        include/atlas/simulation/save.hpp
        include/atlas/simulation/snapshot.hpp
        include/atlas/simulation/table.hpp
        include/atlas/simulation/world.hpp
    DEPENDS
        core
)

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
