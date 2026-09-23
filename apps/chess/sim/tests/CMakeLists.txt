# SPDX-License-Identifier: GPL-3.0-or-later
atlas_add_test(chess_sim
    SOURCES
        test_tables.cpp
        test_world.cpp
    DEPENDS
        atlas::chess_sim
    LABELS
        "unit;determinism"
)
