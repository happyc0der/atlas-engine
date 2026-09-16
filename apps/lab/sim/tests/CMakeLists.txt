# SPDX-License-Identifier: GPL-3.0-or-later
atlas_add_test(lab_sim
    SOURCES
        test_grid_layout.cpp
        test_tables.cpp
        test_generate.cpp
        test_systems.cpp
        test_commands.cpp
        test_snapshot.cpp
        test_save_load.cpp
        test_golden.cpp
    DEPENDS
        atlas::lab_sim
    LABELS
        unit
)
