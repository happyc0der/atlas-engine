# SPDX-License-Identifier: GPL-3.0-or-later
#
# Two targets, mirroring engine/simulation/tests: the sanitizer presets select "unit" and
# "determinism" and nothing else, so anything that must run under ThreadSanitizer — which here
# means anything touching the inbox — has to carry one of those labels or it is a thread test
# that never runs.
atlas_add_test(net
    SOURCES
        test_inbox.cpp
        test_inbox_threads.cpp
        test_link.cpp
        test_enet_hub.cpp
        test_loopback.cpp
        test_message.cpp
        test_protocol.cpp
        test_session.cpp
    DEPENDS
        atlas::net
    LABELS
        "unit"
)

# The lockstep proof runs two kernels in one process and compares them tick by tick. It carries
# the "determinism" label rather than "unit" for the same reason the simulation's own does: the
# sanitizer presets select that label, and a determinism proof no sanitizer runs is a proof with
# a hole in it.
atlas_add_test(net_lockstep
    SOURCES
        test_lockstep.cpp
    DEPENDS
        atlas::net
    LABELS
        "determinism"
)
