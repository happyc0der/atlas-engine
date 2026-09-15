# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::core is not listed: both platform and simulation link it PUBLIC, so it arrives
# transitively. Naming it here as well makes the linker warn about a duplicate library.
atlas_add_app(sandbox
    SOURCES main.cpp
    DEPENDS atlas::platform atlas::simulation
)
