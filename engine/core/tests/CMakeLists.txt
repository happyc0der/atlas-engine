# SPDX-License-Identifier: GPL-3.0-or-later
atlas_add_test(core
    SOURCES
        test_error.cpp
        test_log.cpp
        test_args.cpp
        test_build_info.cpp
    DEPENDS
        atlas::core
    LABELS
        unit
)
