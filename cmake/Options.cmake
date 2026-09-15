# SPDX-License-Identifier: GPL-3.0-or-later
# Project-wide options. Defaults are what a developer building Atlas for the first time
# should get: everything useful, nothing slow.

include(CMakeDependentOption)

option(ATLAS_BUILD_TESTS      "Build the test targets"                         ON)
option(ATLAS_BUILD_APPS       "Build the application targets"                  ON)
option(ATLAS_BUILD_BENCHMARKS "Build the benchmark targets (arrives in M3)"    OFF)
option(ATLAS_PROFILE          "Enable the Tracy profiler client"               OFF)
option(ATLAS_ENABLE_TIDY      "Run clang-tidy as part of the build"            OFF)
option(ATLAS_WARNINGS_AS_ERRORS "Treat first-party warnings as errors"         ON)

set(ATLAS_SANITIZE "none" CACHE STRING
    "Sanitizer to enable for first-party targets: none, address, thread")
set_property(CACHE ATLAS_SANITIZE PROPERTY STRINGS none address thread)

# A single place to report what the configuration actually is, so that a surprising build
# is visible at configure time rather than discovered later.
function(atlas_report_configuration)
    message(STATUS "")
    message(STATUS "Atlas ${PROJECT_VERSION} configuration")
    message(STATUS "  System            : ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
    message(STATUS "  Compiler          : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    message(STATUS "  Build type        : ${CMAKE_BUILD_TYPE}")
    message(STATUS "  C++ standard      : ${CMAKE_CXX_STANDARD}")
    message(STATUS "  vcpkg triplet     : ${VCPKG_TARGET_TRIPLET}")
    message(STATUS "  Tests             : ${ATLAS_BUILD_TESTS}")
    message(STATUS "  Apps              : ${ATLAS_BUILD_APPS}")
    message(STATUS "  Benchmarks        : ${ATLAS_BUILD_BENCHMARKS}")
    message(STATUS "  Tracy profiling   : ${ATLAS_PROFILE}")
    message(STATUS "  clang-tidy        : ${ATLAS_ENABLE_TIDY}")
    message(STATUS "  Sanitizer         : ${ATLAS_SANITIZE}")
    message(STATUS "  Warnings as errors: ${ATLAS_WARNINGS_AS_ERRORS}")
    message(STATUS "")
endfunction()
