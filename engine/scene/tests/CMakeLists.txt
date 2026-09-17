# SPDX-License-Identifier: GPL-3.0-or-later
atlas_add_test(scene
    SOURCES
        test_scene.cpp
        test_hierarchy.cpp
        test_serialization.cpp
        test_animation_pose.cpp
    DEPENDS
        atlas::scene
    LABELS
        unit
)
