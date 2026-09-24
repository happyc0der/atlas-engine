# SPDX-License-Identifier: GPL-3.0-or-later
#
# Chess: the first game on the engine, and a probe of it (ADR-0018). It is the one recorded
# exception to the rule above this directory that applications are not games. Its whole
# purpose is to find what a game needs that the engine lacks, and to find it without a line of
# chess reaching engine/.
#
# The rules are a library that may link only atlas::simulation, fenced at configure time in
# sim/CMakeLists.txt exactly as the lab's simulation is. The view is a second library that turns
# a position into quads, with no device of its own, so its tests need none. The application is
# the composition root that puts a window, a board and two people around them.
add_subdirectory(sim)
add_subdirectory(view)

atlas_add_app(chess
    SOURCES
        main.cpp
    DEPENDS
        atlas::app_common
        atlas::app_lockstep
        atlas::chess_sim
        atlas::chess_view
        atlas::assets
        atlas::net
        atlas::platform
        atlas::renderer
        atlas::rhi
        atlas::text
        atlas::tools
)
