# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::chess_sim — the whole of chess as simulation state and one command, and nothing else:
# no window, no socket, no text. It links atlas::simulation and that is checked below rather
# than trusted, for the reason ADR-0018 D3 gives: the probe proves something only if the game
# could not have reached anything but the engine's simulation contract. The lab's fence at
# apps/lab/sim/CMakeLists.txt is the precedent, copied rather than shared because two
# configure-time loops are cheaper to read than one function with a parameter.
atlas_add_app_library(chess_sim
    SOURCES
        src/tables.cpp
        src/world.cpp
        src/position.cpp
        src/fen.cpp
        src/rules.cpp
    DEPENDS
        atlas::simulation
)

if(TARGET atlas_chess_sim)
    get_target_property(_atlas_chess_sim_links atlas_chess_sim LINK_LIBRARIES)
    foreach(item IN LISTS _atlas_chess_sim_links)
        if(item MATCHES "^atlas::" AND NOT item STREQUAL "atlas::simulation")
            message(FATAL_ERROR
                "atlas_chess_sim may link only atlas::simulation; it links ${item}. "
                "The rules of chess reach the engine through its simulation contract and "
                "nothing else (ADR-0018 D3).")
        endif()
    endforeach()

    if(ATLAS_BUILD_TESTS)
        add_subdirectory(tests)
    endif()
endif()
