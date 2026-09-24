# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::app_common — what every Atlas application needs and no engine module should own.
#
# Extracted when the second application arrived and it turned out that what it shared with the
# first was these utilities, not the composition around them. Each piece was copied character
# for character between the two, and none had a unit test until it moved here.
atlas_add_app_library(app_common
    SOURCES
        src/log_options.cpp
        src/frame_counters.cpp
        src/ppm.cpp
        src/run_bounds.cpp
        src/camera_controller.cpp
    DEPENDS
        atlas::core
        atlas::math
        atlas::platform
        atlas::rhi
)

# atlas::app_lockstep — opening a lockstep session over a socket, the same way in every
# application that plays one. Its own target because it links atlas::net, and an application
# that plays no session should not link a socket library to get a camera controller. Lifted
# out of the lab when chess became the second application to need it (M22).
atlas_add_app_library(app_lockstep
    SOURCES
        src/socket_session.cpp
    DEPENDS
        atlas::core
        atlas::net
        atlas::simulation
)

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
