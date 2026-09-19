# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::text — string tables, one lookup, one substituter (ADR-0016).
#
# Depends on assets because a string table is an asset like any other, and the catalog finalises
# one the way the texture and clip caches do. It must never depend on scene, renderer, platform
# or simulation: resolving a key is not presentation and is certainly not a tick.
#
# The substituter is a free function over plain strings in its own header, so every malformed
# input is tested with no catalog, no registry and no file — the separation the mixer made in
# M12, the camera controller in M11, and the clip evaluator in M13.
atlas_add_module(text
    SOURCES
        src/catalog.cpp
        src/substitute.cpp
    HEADERS
        include/atlas/text/catalog.hpp
        include/atlas/text/substitute.hpp
    DEPENDS
        core
        assets
)

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
