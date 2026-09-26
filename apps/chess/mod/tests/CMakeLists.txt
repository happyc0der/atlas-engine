# SPDX-License-Identifier: GPL-3.0-or-later
atlas_add_test(chess_mod
    SOURCES
        test_opponent_module.cpp
    DEPENDS
        atlas::script
        atlas::simulation
    LABELS
        "unit"
)
