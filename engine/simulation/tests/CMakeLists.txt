# SPDX-License-Identifier: GPL-3.0-or-later
atlas_add_test(simulation
    SOURCES
        test_tick_accumulator.cpp
    DEPENDS
        atlas::simulation
    LABELS
        unit
)
