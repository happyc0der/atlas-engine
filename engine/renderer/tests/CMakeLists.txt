# SPDX-License-Identifier: GPL-3.0-or-later
#
# Labelled "gpu": a quad batch owns real graphics resources, so these need a device.
atlas_add_test(renderer
    SOURCES
        test_quad_batch.cpp
    DEPENDS
        atlas::renderer
        atlas::platform
    LABELS
        "gpu"
)
