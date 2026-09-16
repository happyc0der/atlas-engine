# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::lab_sim — the lab's simulation: synthetic tables, mock systems, one command, and the
# snapshot the renderer reads. It links atlas::simulation and nothing else, and that is checked
# below rather than trusted, because "the headless benchmark is independent of rendering" is
# only true if this library cannot reach a renderer. tools/check_module_deps.py scans engine/
# only, so the check has to live here.
atlas_add_app_library(lab_sim
    SOURCES
        src/grid_layout.cpp
        src/tables.cpp
        src/generate.cpp
        src/systems.cpp
        src/commands.cpp
        src/snapshot.cpp
    DEPENDS
        atlas::simulation
)

if(TARGET atlas_lab_sim)
    get_target_property(_atlas_lab_sim_links atlas_lab_sim LINK_LIBRARIES)
    foreach(item IN LISTS _atlas_lab_sim_links)
        if(item MATCHES "^atlas::" AND NOT item STREQUAL "atlas::simulation")
            message(FATAL_ERROR
                "atlas_lab_sim may link only atlas::simulation; it links ${item}. "
                "The headless simulation benchmark must stay independent of rendering.")
        endif()
    endforeach()

    if(ATLAS_BUILD_TESTS)
        add_subdirectory(tests)
    endif()
endif()
