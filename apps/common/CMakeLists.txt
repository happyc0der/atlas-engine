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

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
