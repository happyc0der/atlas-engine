# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::chess_view — a position as quads. It creates nothing on a device: the application owns
# the batcher and the texture and hands this library a place to write, so every test of what the
# board looks like runs without a graphics device.
atlas_add_app_library(chess_view
    SOURCES
        src/board_layout.cpp
        src/board_quads.cpp
    DEPENDS
        atlas::chess_sim
        atlas::renderer
        atlas::math
)

if(TARGET atlas_chess_view AND ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
