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
        test_save.cpp
        test_snapshot.cpp
        test_turn_gate.cpp
        test_command_codec.cpp
        test_divergence.cpp
    DEPENDS
        atlas::simulation
    LABELS
        unit
)

# The determinism tests are labelled separately so they can be selected on their own: the
# sanitizer presets run this label and "unit" and nothing else, because SDL's internals are
# uninstrumented noise under ThreadSanitizer.
atlas_add_test(determinism
    SOURCES
        test_determinism.cpp
        test_golden.cpp
    DEPENDS
        atlas::simulation
    LABELS
        "determinism"
)
