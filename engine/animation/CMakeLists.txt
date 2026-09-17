# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::animation — clip evaluation, and the derived pose it writes.
#
# Depends on scene because the pose is a scene component, and on assets because a clip is an
# asset like any other. It must never depend on renderer or simulation: animation is
# presentation, nothing it produces is hashed, and what draws the result is not its concern.
#
# The evaluation is pure functions over plain data in their own header, so the arithmetic is
# tested with no scene, no device and no clock — the same separation the mixer made in M12 and
# the camera controller in M11.
atlas_add_module(animation
    SOURCES
        src/clip.cpp
        src/advance.cpp
    HEADERS
        include/atlas/animation/advance.hpp
        include/atlas/animation/clip.hpp
    DEPENDS
        core
        math
        assets
        scene
)

add_subdirectory(tests)
