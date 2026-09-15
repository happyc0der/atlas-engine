# SPDX-License-Identifier: GPL-3.0-or-later
atlas_add_test(math
    SOURCES
        test_vector.cpp
        test_matrix.cpp
        test_camera.cpp
    DEPENDS
        atlas::math
    LABELS
        unit
)
