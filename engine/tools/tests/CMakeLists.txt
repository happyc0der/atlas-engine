# SPDX-License-Identifier: GPL-3.0-or-later
atlas_add_test(tools
    SOURCES
        test_scene_panel.cpp
    DEPENDS
        atlas::tools
        atlas::edit
        atlas::platform
    LABELS
        "gpu"
)
