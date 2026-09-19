# SPDX-License-Identifier: GPL-3.0-or-later
#
# There is deliberately no test asserting that this module throws nothing.
# `tools/check_module_deps.py` already refuses `throw` or `catch` in any non-test engine file
# without an entry in its exception allow-list, repo-wide and mechanically. A test could only
# check the cases it thought of.
atlas_add_test(text
    SOURCES
        test_substitute.cpp
        test_catalog.cpp
    DEPENDS
        atlas::text
    LABELS
        "unit"
)
