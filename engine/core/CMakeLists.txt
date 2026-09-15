# SPDX-License-Identifier: GPL-3.0-or-later
#
# atlas::core — foundations every other module depends on.
#
# Its only dependency is the C++ standard library. Tracy appears in profile.hpp as macros
# that compile to nothing unless ATLAS_PROFILE is on.

# Build identity. Captured at configure time so that every artifact can say which commit
# produced it. A dirty tree is recorded rather than hidden: a benchmark result from
# uncommitted changes is not reproducible and should say so.
find_package(Git QUIET)
set(ATLAS_GIT_COMMIT "unknown")
set(ATLAS_GIT_DIRTY "false")
set(ATLAS_GIT_DIRTY_SUFFIX "")

if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        OUTPUT_VARIABLE ATLAS_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    if(NOT _git_status STREQUAL "")
        set(ATLAS_GIT_DIRTY "true")
        set(ATLAS_GIT_DIRTY_SUFFIX "-dirty")
    endif()

    if(ATLAS_GIT_COMMIT STREQUAL "")
        set(ATLAS_GIT_COMMIT "unknown")
    endif()
endif()

string(TIMESTAMP ATLAS_CONFIGURE_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/build_info.cpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.cpp"
    @ONLY)

atlas_add_module(core
    SOURCES
        src/error.cpp
        src/assert.cpp
        src/log.cpp
        src/args.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.cpp"
    HEADERS
        include/atlas/core/args.hpp
        include/atlas/core/assert.hpp
        include/atlas/core/build_info.hpp
        include/atlas/core/error.hpp
        include/atlas/core/log.hpp
        include/atlas/core/profile.hpp
        include/atlas/core/result.hpp
)

if(ATLAS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
