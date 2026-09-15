# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::core is not listed: the modules below link it PUBLIC, so it arrives transitively.
# Naming it here as well makes the linker warn about a duplicate library.
atlas_add_app(sandbox
    SOURCES
        main.cpp
        scene.cpp
    DEPENDS
        atlas::assets
        atlas::platform
        atlas::renderer
        atlas::simulation
        atlas::tools
)
