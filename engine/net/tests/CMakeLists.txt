# SPDX-License-Identifier: GPL-3.0-or-later
#
# Two targets, mirroring engine/simulation/tests: the sanitizer presets select "unit" and
# "determinism" and nothing else, so anything that must run under ThreadSanitizer — which here
# means anything touching the inbox — has to carry one of those labels or it is a thread test
# that never runs.
atlas_add_test(net
    SOURCES
        test_protocol.cpp
    DEPENDS
        atlas::net
    LABELS
        "unit"
)
