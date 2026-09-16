# SPDX-License-Identifier: GPL-3.0-or-later
atlas_add_test(lab_view
    SOURCES
        test_cell_field.cpp
        test_cell_id_pass.cpp
    DEPENDS
        atlas::lab_view
        atlas::platform
    LABELS
        "gpu"
)
