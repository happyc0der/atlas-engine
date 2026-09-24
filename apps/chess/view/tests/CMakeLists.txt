# SPDX-License-Identifier: GPL-3.0-or-later
atlas_add_test(chess_view
    SOURCES
        test_board_layout.cpp
        test_board_quads.cpp
    DEPENDS
        atlas::chess_view
    LABELS
        "unit"
)
