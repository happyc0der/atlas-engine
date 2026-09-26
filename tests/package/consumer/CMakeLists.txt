# SPDX-License-Identifier: GPL-3.0-or-later
#
# A project that is not Atlas, built against an installed Atlas (ADR-0024 D10).
#
# It is configured by tests/package/run.py with no vcpkg toolchain and no package registry, so
# everything it knows of Atlas comes through find_package from an install prefix and the vcpkg
# tree beside it. It includes none of Atlas's CMake: its warnings are its own, as another
# project's would be. run.py then checks that no include path it compiled with points into the
# Atlas source tree, and runs it.
#
#   atlas_consumer          uses a module of each kind: a simulation, a script runtime, a
#                           loopback link, a string table read from Atlas_DATA_DIR, the app kit
#   atlas_consumer_headers  compiles every installed header on its own, twice over, so a header
#                           that leans on something its includer happened to include first fails
#
# ATLAS_CONSUMER_MUTATION breaks one thing on purpose, for the cases that prove each check can
# fail. Empty in the case that proves the package works.
cmake_minimum_required(VERSION 3.28)
project(AtlasConsumer LANGUAGES CXX)

set(ATLAS_CONSUMER_HEADERS "" CACHE FILEPATH
    "A file naming every public header Atlas should have installed, one per line")
set(ATLAS_CONSUMER_MUTATION "" CACHE STRING
    "Break one thing on purpose: old_version, internal_target, internal_header, source_include, wrong_golden")
set(ATLAS_CONSUMER_SOURCE_INCLUDE "" CACHE PATH
    "For source_include: an include directory in the Atlas source tree")

set(_atlas_version 0.27)
if(ATLAS_CONSUMER_MUTATION STREQUAL "old_version")
    set(_atlas_version 0.26)
endif()
find_package(Atlas ${_atlas_version} REQUIRED COMPONENTS app)
message(STATUS "AtlasConsumer: Atlas ${Atlas_VERSION} from ${Atlas_DIR}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Atlas is headers, not C++20 modules. Left on, CMake scans every source for modules under Ninja
# with clang or GCC, which needs clang-scan-deps and fails without it. Atlas turns the scan off for
# itself, but that is a directory setting a package cannot carry, so a consumer turns it off too
# (ADR-0024 D9). Found by the Linux container, whose clang has no scanner; AppleClang is never
# scanned, which is why macOS did not show it.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Atlas's public headers are compiled here under warnings another project might well use, so a
# header that is clean only under Atlas's own flags fails. With MSVC this is also what would show
# a header needing _CRT_SECURE_NO_WARNINGS, which Atlas defines for itself and nobody else.
#
# NO_SYSTEM_FROM_IMPORTED: an imported target's include directories are otherwise passed as system
# directories (-isystem, /external:I), where no warning is reported at all, and every one of
# Atlas's headers would pass whatever it contained.
function(consumer_warnings target)
    set_target_properties(${target} PROPERTIES NO_SYSTEM_FROM_IMPORTED ON)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Werror)
    endif()
endfunction()

set(_atlas_everything
    atlas::core atlas::math atlas::platform atlas::tasks atlas::rhi atlas::renderer atlas::assets
    atlas::audio atlas::scene atlas::edit atlas::animation atlas::simulation atlas::net
    atlas::script atlas::text atlas::tools atlas::app_common atlas::app_lockstep atlas::app_mods)

add_executable(atlas_consumer main.cpp)
target_link_libraries(atlas_consumer PRIVATE
    atlas::simulation atlas::script atlas::net atlas::assets atlas::text atlas::tools
    atlas::app_common)
target_compile_definitions(atlas_consumer PRIVATE
    "ATLAS_CONSUMER_DATA_DIR=\"${Atlas_DATA_DIR}\"")
consumer_warnings(atlas_consumer)

if(ATLAS_CONSUMER_MUTATION STREQUAL "internal_target")
    target_link_libraries(atlas_consumer PRIVATE atlas::rhi_internal)
elseif(ATLAS_CONSUMER_MUTATION STREQUAL "internal_header")
    target_sources(atlas_consumer PRIVATE mutation_internal_header.cpp)
elseif(ATLAS_CONSUMER_MUTATION STREQUAL "source_include")
    target_include_directories(atlas_consumer PRIVATE "${ATLAS_CONSUMER_SOURCE_INCLUDE}")
elseif(ATLAS_CONSUMER_MUTATION STREQUAL "wrong_golden")
    target_compile_definitions(atlas_consumer PRIVATE ATLAS_CONSUMER_WRONG_GOLDEN=1)
elseif(ATLAS_CONSUMER_MUTATION)
    message(FATAL_ERROR "AtlasConsumer: no mutation named '${ATLAS_CONSUMER_MUTATION}'")
endif()

# The executable finds its DLLs beside it. On the Windows triplet SDL3 and its neighbours are
# shared libraries; everywhere else every library is static and this list is empty.
if(WIN32)
    add_custom_command(TARGET atlas_consumer POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                $<TARGET_RUNTIME_DLLS:atlas_consumer> $<TARGET_FILE_DIR:atlas_consumer>
        COMMAND_EXPAND_LISTS)
endif()

# One translation unit per header, generated from the list run.py writes from the source tree.
# A header left out of the install therefore fails here rather than going unnoticed.
if(NOT ATLAS_CONSUMER_HEADERS)
    message(FATAL_ERROR "AtlasConsumer: pass ATLAS_CONSUMER_HEADERS; run.py writes it")
endif()
file(STRINGS "${ATLAS_CONSUMER_HEADERS}" _atlas_headers)
set(_atlas_header_sources "")
foreach(_header IN LISTS _atlas_headers)
    string(MAKE_C_IDENTIFIER "${_header}" _name)
    set(_source "${CMAKE_CURRENT_BINARY_DIR}/headers/${_name}.cpp")
    # Twice, so a header without an include guard fails too.
    file(CONFIGURE OUTPUT "${_source}" CONTENT
        "// Generated by tests/package/consumer/CMakeLists.txt.\n#include <${_header}>\n#include <${_header}>\n")
    list(APPEND _atlas_header_sources "${_source}")
endforeach()
list(LENGTH _atlas_header_sources _count)
message(STATUS "AtlasConsumer: compiling ${_count} installed headers on their own")
add_library(atlas_consumer_headers OBJECT ${_atlas_header_sources})
target_link_libraries(atlas_consumer_headers PRIVATE ${_atlas_everything})
consumer_warnings(atlas_consumer_headers)
