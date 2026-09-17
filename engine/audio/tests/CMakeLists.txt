# SPDX-License-Identifier: GPL-3.0-or-later
#
# The mixer and the generator need no device, so they are "unit" and run everywhere, including
# under the sanitizers. The device tests ask for the dummy driver and SKIP if they cannot get
# it, following engine/platform/tests/test_window_dummy.cpp: a machine with no audio at all
# should report a skip, not a failure.
atlas_add_test(audio
    SOURCES
        test_mix.cpp
        test_synth.cpp
        test_device.cpp
    DEPENDS
        atlas::audio
        atlas::platform
    LABELS
        "unit"
)
