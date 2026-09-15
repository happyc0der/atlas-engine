# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::math — vectors, matrices, and the two-dimensional camera.
#
# Presentation only. Simulation state is integer or fixed-point, for the reasons in
# docs/DETERMINISM.md, so nothing here is on a determinism-critical path.
atlas_add_module(math
    SOURCES
        src/matrix.cpp
        src/camera.cpp
    HEADERS
        include/atlas/math/camera.hpp
        include/atlas/math/matrix.hpp
        include/atlas/math/vector.hpp
    DEPENDS
        core
)

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
