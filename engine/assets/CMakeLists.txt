# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::assets — virtual paths, the mounted filesystem, asset identifiers and loading.
#
# Knows nothing about the graphics device: an importer produces bytes and descriptions, and
# whoever owns the device turns those into resources on the thread that is allowed to.
# stb is a header-only decoder installed as a bare include directory rather than a CMake
# package, so it is found by path rather than by find_package.
find_path(STB_INCLUDE_DIR NAMES stb_image.h REQUIRED)

atlas_add_module(assets
    SOURCES
        src/virtual_path.cpp
        src/asset_id.cpp
        src/importer.cpp
        src/wav.cpp
        src/audio_import.cpp
        src/registry.cpp
        src/artifact_cache.cpp
        src/filesystem.cpp
    HEADERS
        include/atlas/assets/artifact_cache.hpp
        include/atlas/assets/asset_id.hpp
        include/atlas/assets/asset_state.hpp
        include/atlas/assets/filesystem.hpp
        include/atlas/assets/importer.hpp
        include/atlas/assets/registry.hpp
        include/atlas/assets/virtual_path.hpp
    DEPENDS
        core
        platform
)

# SYSTEM, so that the decoder's own warnings are not held to Atlas's first-party standard.
# It is C from another project and is not going to stop using old-style casts because this
# build asked it to.
target_include_directories(atlas_assets SYSTEM PRIVATE ${STB_INCLUDE_DIR})

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
