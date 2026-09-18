# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::core is not listed: the modules below link it PUBLIC, so it arrives transitively.
# Naming it here as well makes the linker warn about a duplicate library.
atlas_add_app(sandbox
    SOURCES
        main.cpp
        scene.cpp
        scene_demo.cpp
    DEPENDS
        atlas::animation
        atlas::app_common
        atlas::assets
        atlas::edit
        atlas::audio
        atlas::platform
        atlas::renderer
        atlas::scene
        atlas::simulation
        atlas::tools
)
