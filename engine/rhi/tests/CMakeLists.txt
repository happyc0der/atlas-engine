# SPDX-License-Identifier: GPL-3.0-or-later
#
# Labelled "gpu": these need a real graphics device, which hosted CI runners do not have.
# `ctest --preset <name>` excludes them; `--preset <name>-gpu` includes them.
atlas_add_test(rhi
    SOURCES
        test_types.cpp
        test_device.cpp
        test_render.cpp
    DEPENDS
        atlas::rhi
    LABELS
        "gpu"
)

# Failure paths that need no device at all, so they run everywhere.
atlas_add_test(rhi_headless
    SOURCES
        test_device_headless.cpp
    DEPENDS
        atlas::rhi
    LABELS
        "display"
)
