# SPDX-License-Identifier: GPL-3.0-or-later
atlas_add_test(edit
    SOURCES
        test_commands.cpp
        test_history.cpp
    DEPENDS
        atlas::edit
    LABELS
        "unit"
)
