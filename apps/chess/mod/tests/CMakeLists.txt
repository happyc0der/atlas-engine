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
        # The rules library's test harness, beside this test in the chess tree.
        "${CMAKE_CURRENT_SOURCE_DIR}/../../sim/tests"
    LABELS
        "unit"
)
