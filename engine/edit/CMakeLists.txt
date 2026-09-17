# SPDX-License-Identifier: GPL-3.0-or-later
# atlas::edit — undoable edits to a scene, and the history that applies them.
#
# Between scene and tools rather than inside either. Inside scene, the data model would know
# it is being edited and every future editor concept would become a scene concern. Inside
# tools, a test of an edit command would link Dear ImGui and SDL, which is why tools has no
# tests at all; these must run headless.
atlas_add_module(edit
    SOURCES
        src/command.cpp
        src/history.cpp
    HEADERS
        include/atlas/edit/command.hpp
        include/atlas/edit/history.hpp
    DEPENDS
        core
        scene
)

add_subdirectory(tests)
