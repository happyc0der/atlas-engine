# SPDX-License-Identifier: GPL-3.0-or-later
atlas_add_test(chess_mod
    SOURCES
        test_opponent_module.cpp
        test_rules.cpp
        test_opponent.cpp
    DEPENDS
        atlas::script
        atlas::simulation
        atlas::chess_sim
        atlas_chess_mod_rules
    INCLUDE_DIRS
        "${PROJECT_SOURCE_DIR}/apps/chess/sim/tests"
    LABELS
        "unit"
)
