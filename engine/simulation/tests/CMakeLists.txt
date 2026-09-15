# SPDX-License-Identifier: GPL-3.0-or-later
atlas_add_test(simulation
    SOURCES
        test_tick_accumulator.cpp
        test_save_stream.cpp
        test_world.cpp
        test_rng.cpp
        test_schedule.cpp
        test_command.cpp
        test_kernel.cpp
    DEPENDS
        atlas::simulation
    LABELS
        unit
)
