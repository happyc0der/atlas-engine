# SPDX-License-Identifier: GPL-3.0-or-later
atlas_add_test(assets
    SOURCES
        test_virtual_path.cpp
        test_filesystem.cpp
        test_registry.cpp
    DEPENDS
        atlas::assets
    LABELS
        unit
)
