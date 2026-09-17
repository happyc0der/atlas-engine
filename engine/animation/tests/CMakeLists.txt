# SPDX-License-Identifier: GPL-3.0-or-later
atlas_add_test(animation
    SOURCES
        test_clip.cpp
        test_advance.cpp
        test_clip_assets.cpp
    DEPENDS
        atlas::animation
    LABELS
        "unit"
)
