# SPDX-License-Identifier: GPL-3.0-or-later
#
# The chess opponent, written as a sandboxed mod in freestanding C (ADR-0023).
#
# The C here is not built by CMake for the game: `tools/build_mods.py` compiles it to wasm32 and
# commits the module, so nobody running the chess application needs a WebAssembly toolchain,
# exactly as nobody drawing a frame needs a shader compiler. What CMake builds is the proof that
# the committed module loads and runs in the engine's runtime under its unchanged limits.
if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
