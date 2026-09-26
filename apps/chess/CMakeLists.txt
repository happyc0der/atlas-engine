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
add_subdirectory(mod)

atlas_add_app(chess
    SOURCES
        main.cpp
    DEPENDS
        atlas::app_common
        atlas::app_lockstep
        atlas::app_mods
        atlas::chess_sim
        atlas::chess_view
        atlas::assets
        atlas::net
        atlas::platform
        atlas::renderer
        atlas::script
        atlas::rhi
        atlas::text
        atlas::tools
)

# Where the engine's own data is, for this build (ADR-0024 D8): this repository's asset folders.
# Built against an installed Atlas, the same two point into its Atlas_DATA_DIR instead.
target_compile_definitions(atlas_chess PRIVATE
    ATLAS_CHESS_SHADER_DIR="assets/cooked/shaders"
    ATLAS_CHESS_ENGINE_STRINGS_DIR="assets/source/strings")

# The piece sheet is chess's, and so is its generator (M27, ADR-0024): checked here, as the
# engine's textures are checked beside the engine.
if(ATLAS_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME chess_textures_current
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_chess_textures.py" --check)
    set_tests_properties(chess_textures_current PROPERTIES LABELS "lint")
endif()

# And its mod, listed in mods.json and built by the engine's tools/build_mods.py (ADR-0023,
# ADR-0024 D8), compared in play against the committed module by this application itself.
if(ATLAS_BUILD_TESTS AND Python3_Interpreter_FOUND)
    add_test(NAME chess_mods_built_current
             COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/build_mods.py"
                     --mods apps/chess/mods.json --root "${PROJECT_SOURCE_DIR}" --check
                     "--play-with=$<TARGET_FILE:atlas_chess>")
    set_tests_properties(chess_mods_built_current PROPERTIES LABELS "lint")
endif()
