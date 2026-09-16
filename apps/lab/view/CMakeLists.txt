# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::lab_view — the cell field and the identifier pass. A library rather than application
# sources so that the GPU tests can drive exactly the code the application draws with.
atlas_add_app_library(lab_view
    SOURCES
        src/cell_field.cpp
        src/cell_id_pass.cpp
    DEPENDS
        atlas::lab_sim
        atlas::math
        atlas::platform
        atlas::renderer
        atlas::rhi
)
