# SPDX-License-Identifier: GPL-3.0-or-later
#
# Labelled determinism as well as unit: the thread-sanitizer preset runs those two labels, and a
# worker pool is exactly the thing that preset exists for.
atlas_add_test(tasks
    SOURCES
        test_worker_pool.cpp
    DEPENDS
        atlas::tasks
    LABELS
        "unit;determinism"
)
