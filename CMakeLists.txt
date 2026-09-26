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

    # MSVC deprecates standard C functions in favour of its own _s variants, which are not
    # portable. Atlas uses the standard ones and checks their results; the warning is about
    # not using Microsoft's alternative, not about a mistake, and silencing it is the only
    # way to keep one portable implementation.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
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

# The boundary and licence checks run as tests as well as in continuous integration, so
# that a local `ctest` catches a violation at the moment it is introduced rather than after
# a push. Labelled "lint" so they can be excluded from a sanitizer run, where they would
# only be repeating themselves.
if(ATLAS_BUILD_TESTS)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME module_boundaries
                 COMMAND "${Python3_EXECUTABLE}"
                         "${PROJECT_SOURCE_DIR}/tools/check_module_deps.py")
        set_tests_properties(module_boundaries PROPERTIES LABELS "lint")

        add_test(NAME licence_headers
                 COMMAND "${Python3_EXECUTABLE}"
                         "${PROJECT_SOURCE_DIR}/tools/check_spdx.py")
        set_tests_properties(licence_headers PROPERTIES LABELS "lint")

        # The vcpkg pin, recorded three times, names one commit (M23). The binary cache is keyed
        # on vcpkg.json, so this is what makes that key follow the submodule too.
        add_test(NAME vcpkg_pin
                 COMMAND "${Python3_EXECUTABLE}"
                         "${PROJECT_SOURCE_DIR}/tools/check_vcpkg_pin.py")
        set_tests_properties(vcpkg_pin PROPERTIES LABELS "lint")

        # Skips itself, rather than failing, on a machine without the shader toolchain: a
        # machine that cannot compile shaders is a machine that cannot have changed them.
        add_test(NAME shaders_current
                 COMMAND "${Python3_EXECUTABLE}"
                         "${PROJECT_SOURCE_DIR}/tools/cook_shaders.py" --check)
        set_tests_properties(shaders_current PROPERTIES LABELS "lint")

        # The audio assets are generated the same way and for the same reason: a sound file of
        # unknown provenance is one nobody can regenerate, relicense or explain. Unlike the
        # shaders this needs no toolchain, so it never skips.
        add_test(NAME audio_assets_current
                 COMMAND "${Python3_EXECUTABLE}"
                         "${PROJECT_SOURCE_DIR}/tools/gen_audio_assets.py" --check)
        set_tests_properties(audio_assets_current PROPERTIES LABELS "lint")

        # The textures likewise, added in M13 when the sprite sheet arrived. Its generator
        # writes the PNG pixel data as uncompressed deflate blocks on purpose: a byte
        # comparison against a file produced by another machine's zlib would be comparing
        # compression libraries rather than content, which is precisely how the shader
        # currency check above came to pass by coincidence for six milestones.
        add_test(NAME textures_current
                 COMMAND "${Python3_EXECUTABLE}"
                         "${PROJECT_SOURCE_DIR}/tools/gen_textures.py" --check)
        set_tests_properties(textures_current PROPERTIES LABELS "lint")

        # And the demonstration mod, added in M15. Its generator writes WebAssembly bytes
        # directly rather than calling an assembler, for the same reason the textures avoid
        # zlib: a byte comparison against output from a third-party compiler compares tool
        # versions rather than content. ADR-0015 left the choice open and this is the answer.
        add_test(NAME mods_current
                 COMMAND "${Python3_EXECUTABLE}"
                         "${PROJECT_SOURCE_DIR}/tools/gen_mods.py" --check)
        set_tests_properties(mods_current PROPERTIES LABELS "lint")

        # And the mods written in C, added in M26 (ADR-0023). Their bytes are the compiler's,
        # so this compares what is this repository's on every machine — every input's hash and
        # the committed module against its manifest — and compares a rebuild's bytes only where
        # the toolchain is the manifest's own, skipping that part loudly anywhere else. The
        # shader check's rule, prescribed for a compiled mod before one existed.
        # Given the chess application where one is built, it also plays the rebuilt module and
        # the committed one against themselves and compares the games, which verifies a module
        # on a machine whose compiler cannot reproduce its bytes.
        add_test(NAME mods_built_current
                 COMMAND "${Python3_EXECUTABLE}"
                         "${PROJECT_SOURCE_DIR}/tools/build_mods.py" --check
                         "$<$<TARGET_EXISTS:atlas_chess>:--play-with=$<TARGET_FILE:atlas_chess>>")
        set_tests_properties(mods_built_current PROPERTIES LABELS "lint")
    else()
        message(STATUS "Atlas: Python not found; the boundary and licence checks will not "
                       "run as tests (CI runs them separately)")
    endif()
endif()

if(ATLAS_BUILD_APPS)
    add_subdirectory(apps)
endif()

# After apps, because the integration tests run the sandbox binary and need its target.
add_subdirectory(tests)

add_subdirectory(benchmarks)

atlas_report_configuration()
