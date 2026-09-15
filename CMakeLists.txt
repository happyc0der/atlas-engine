# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.28)

# Optional dependencies are vcpkg manifest features, so they must be selected before
# project() runs the vcpkg toolchain. ATLAS_PROFILE arrives from the cache (-D or a preset)
# before the option() call below gives it a default, which is why this reads it here.
if(ATLAS_PROFILE)
    list(APPEND VCPKG_MANIFEST_FEATURES "profile")
endif()

# The toolchain file is supplied by CMakePresets.json. Configuring without a preset will
# not find dependencies; see README.md for the quick start.
project(Atlas
    VERSION 0.0.1
    DESCRIPTION "A C++23 engine platform for a large deterministic world simulation"
    HOMEPAGE_URL "https://github.com/happyc0der/atlas"
    LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not supported. Use a preset: cmake --preset macos-debug")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(Options)
include(ModuleGraph)
include(CompilerWarnings)
include(Sanitizers)
include(StaticAnalysis)
include(Tracy)
include(Dependencies)
include(AtlasModule)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Atlas uses headers, not C++20 modules. With Ninja and a C++20-or-later standard, CMake
# otherwise runs a module dependency scan over every source, which needs clang-scan-deps to
# be installed and costs a pass over the whole tree for information that is always empty.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# MSVC has no /std:c++23. /std:c++23preview exists but is explicitly not ABI-stable across
# releases, which is unusable with a binary dependency cache, so c++latest it is.
# See docs/adr/0001-language-build-platform.md.
if(MSVC)
    set(CMAKE_CXX_STANDARD 23)
    add_compile_options(/std:c++latest)
endif()

# Keep build output predictable across generators.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

atlas_configure_tracy()

if(ATLAS_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
    include(CTest)
    include(Catch)
endif()

add_subdirectory(engine)

if(ATLAS_BUILD_APPS)
    add_subdirectory(apps)
endif()

atlas_report_configuration()
