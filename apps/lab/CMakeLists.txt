# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Strategy Lab: the M7 truth test for the engine. Nothing in here is a game.
#
# The simulation is a separate library that may link only atlas::simulation, so that the
# headless benchmark is independent of rendering by construction. The view is a second library
# so the GPU tests draw with the application's own code. The application is the composition
# root that puts a window, a device and an overlay around them.
add_subdirectory(sim)
add_subdirectory(view)

atlas_add_app(lab
    SOURCES
        main.cpp
        file_bytes.cpp
    DEPENDS
        atlas::app_common
        atlas::lab_sim
        atlas::lab_view
        atlas::platform
        atlas::rhi
        atlas::tools
)

if(ATLAS_BUILD_TESTS AND TARGET atlas_lab_view)
    add_subdirectory(tests)
endif()
