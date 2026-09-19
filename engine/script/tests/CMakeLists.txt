# SPDX-License-Identifier: GPL-3.0-or-later
#
# Labelled "unit", which is what the address-sanitiser presets run — and ASan is the point of a
# hostile-input suite: "refused" and "refused without reading past the end of the buffer" are
# different claims, and only one of them can be checked by looking at a return value.
atlas_add_test(script
    SOURCES
        test_runtime.cpp
        test_mod.cpp
        test_mod_host.cpp
    DEPENDS
        atlas::script
    LABELS
        "unit"
)
