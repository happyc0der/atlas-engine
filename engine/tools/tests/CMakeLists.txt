# SPDX-License-Identifier: GPL-3.0-or-later
# What the panels decide, without a device.
atlas_add_test(tools_logic
    SOURCES
        test_panels.cpp
    DEPENDS
        atlas::tools
    LABELS
        "unit"
)

# The panels themselves, which need one.
atlas_add_test(tools
    SOURCES
        test_scene_panel.cpp
        test_stats_panel.cpp
    DEPENDS
        atlas::tools
        atlas::edit
        atlas::text
        atlas::platform
    LABELS
        "gpu"
)
