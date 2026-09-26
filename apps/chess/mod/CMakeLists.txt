# SPDX-License-Identifier: GPL-3.0-or-later
#
# The chess opponent, written as a sandboxed mod in freestanding C (ADR-0023).
#
# The C here is not built by CMake for the game: `tools/build_mods.py`, given
# `apps/chess/mods.json`, compiles it to wasm32 and commits the module, so nobody running the chess
# application needs a WebAssembly toolchain, exactly as nobody drawing a frame needs a shader
# compiler. What CMake builds is the evidence: the rules compiled natively and checked against the
# published perft counts and against `atlas::chess_sim`, and the committed module loaded into the
# engine's runtime.
if(ATLAS_BUILD_TESTS)
    # C for this directory only, because these sources are C and are compiled as C for the mod.
    # Compiling them as C++ here would test a different language from the one that ships.
    enable_language(C)

    # The same rules the module is built from, as a native library the tests can call. Not
    # freestanding here, because a test process has a C library; nothing in these sources uses
    # one either way.
    add_library(atlas_chess_mod_rules STATIC rules.c)
    target_include_directories(atlas_chess_mod_rules PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    set_target_properties(atlas_chess_mod_rules PROPERTIES
        C_STANDARD 17
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        FOLDER "tests")
    if(MSVC)
        target_compile_options(atlas_chess_mod_rules PRIVATE /W4 /WX)
    else()
        target_compile_options(atlas_chess_mod_rules PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror)
    endif()
    atlas_apply_sanitizers(atlas_chess_mod_rules)

    add_subdirectory(tests)
endif()
