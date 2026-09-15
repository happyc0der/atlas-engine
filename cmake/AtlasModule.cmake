# SPDX-License-Identifier: GPL-3.0-or-later
# atlas_add_module: the only sanctioned way to create an engine module.
#
# It creates the library and its alias, applies warnings, flags, sanitizers, and tidy, and
# rejects any dependency edge that is not in cmake/ModuleGraph.cmake. Using add_library
# directly under engine/ bypasses that check and is forbidden by CLAUDE.md.
#
#   atlas_add_module(core
#       SOURCES      src/log.cpp src/error.cpp
#       HEADERS      include/atlas/core/log.hpp
#       DEPENDS      # other Atlas modules, linked PUBLIC
#       PRIVATE_DEPS # third-party targets, linked PRIVATE
#   )
#
# Public headers live in engine/<name>/include/atlas/<name>/ and are included as
# <atlas/<name>/thing.hpp>. Private headers live beside the sources in src/.

include_guard(GLOBAL)

function(atlas_add_module name)
    cmake_parse_arguments(ARG
        "INTERFACE"
        ""
        "SOURCES;HEADERS;DEPENDS;PRIVATE_DEPS"
        ${ARGN})

    if(NOT name IN_LIST ATLAS_MODULES)
        message(FATAL_ERROR
            "atlas_add_module: '${name}' is not a known module. Add it to "
            "cmake/ModuleGraph.cmake, with its permitted dependencies and a reason.")
    endif()

    # Every declared dependency must be permitted by the allow-list. This is the boundary
    # check that runs before anything is built.
    foreach(dep IN LISTS ARG_DEPENDS)
        if(NOT dep IN_LIST ATLAS_MODULE_DEPS_${name})
            message(FATAL_ERROR
                "atlas_add_module(${name}): dependency on 'atlas::${dep}' is not permitted.\n"
                "  Permitted: ${ATLAS_MODULE_DEPS_${name}}\n"
                "  Adding this edge means editing cmake/ModuleGraph.cmake and writing down "
                "why the boundary should move.")
        endif()
    endforeach()

    set(target atlas_${name})

    if(ARG_INTERFACE)
        add_library(${target} INTERFACE)
        add_library(atlas::${name} ALIAS ${target})
        target_include_directories(${target} INTERFACE
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>)
        target_compile_features(${target} INTERFACE cxx_std_23)
        foreach(dep IN LISTS ARG_DEPENDS)
            target_link_libraries(${target} INTERFACE atlas::${dep})
        endforeach()
        foreach(dep IN LISTS ARG_PRIVATE_DEPS)
            target_link_libraries(${target} INTERFACE ${dep})
        endforeach()
        return()
    endif()

    add_library(${target} STATIC ${ARG_SOURCES} ${ARG_HEADERS})
    add_library(atlas::${name} ALIAS ${target})

    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)

    target_compile_features(${target} PUBLIC cxx_std_23)

    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF          # -std=c++23, never -std=gnu++23.
        POSITION_INDEPENDENT_CODE ON
        FOLDER "engine")

    # Atlas modules link PUBLIC: a consumer of atlas::renderer legitimately sees
    # atlas::core types in its headers.
    foreach(dep IN LISTS ARG_DEPENDS)
        target_link_libraries(${target} PUBLIC atlas::${dep})
    endforeach()

    # Third-party libraries link PRIVATE. For static libraries this propagates as
    # $<LINK_ONLY:>, so their include directories do not reach consumers and a public
    # header that includes one fails to compile downstream. That is the enforcement;
    # tools/check_module_deps.py is the backstop.
    foreach(dep IN LISTS ARG_PRIVATE_DEPS)
        target_link_libraries(${target} PRIVATE ${dep})
    endforeach()

    atlas_set_warnings(${target})
    atlas_set_common_compile_options(${target})
    atlas_apply_sanitizers(${target})
    atlas_enable_tidy(${target})
    atlas_link_tracy(${target})
endfunction()

# atlas_add_test: a Catch2 test executable for one module.
#
# LABELS map to CTest labels and are how the presets select what to run: hosted CI excludes
# "gpu", and sanitizer presets include only "unit" and "determinism".
# PRIVATE_DEPS and INCLUDE_DIRS exist for one legitimate case: a test of the code that sits
# directly on a third-party boundary, such as the SDL scancode mapping. That test genuinely
# needs the third-party header, and it is a test rather than a public header, so the rule
# it would otherwise break does not apply to it.
function(atlas_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS;PRIVATE_DEPS;INCLUDE_DIRS;LABELS" ${ARGN})

    if(NOT ATLAS_BUILD_TESTS)
        return()
    endif()

    set(target atlas_test_${name})
    add_executable(${target} ${ARG_SOURCES})

    target_link_libraries(${target} PRIVATE Catch2::Catch2WithMain)
    foreach(dep IN LISTS ARG_DEPENDS)
        target_link_libraries(${target} PRIVATE ${dep})
    endforeach()
    foreach(dep IN LISTS ARG_PRIVATE_DEPS)
        target_link_libraries(${target} PRIVATE ${dep})
    endforeach()
    foreach(dir IN LISTS ARG_INCLUDE_DIRS)
        target_include_directories(${target} PRIVATE ${dir})
    endforeach()

    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        FOLDER "tests")

    atlas_set_warnings(${target})
    atlas_set_common_compile_options(${target})
    atlas_apply_sanitizers(${target})

    if(NOT ARG_LABELS)
        set(ARG_LABELS unit)
    endif()

    # PRE_TEST discovery rather than POST_BUILD: on Apple Silicon a freshly linked binary
    # may not be runnable at build time because of code signing, and POST_BUILD discovery
    # would fail. Catch2 documents this case.
    #
    # The working directory is the project root so that a relative asset path means the same
    # thing under CTest as it does when the binary is run by hand. Depending on the caller's
    # working directory is a stopgap; the asset system replaces it with virtual paths in M4.
    catch_discover_tests(${target}
        DISCOVERY_MODE PRE_TEST
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        PROPERTIES LABELS "${ARG_LABELS}")
endfunction()

# atlas_add_app: an application target. Applications are composition roots, never libraries.
function(atlas_add_app name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS" ${ARGN})

    if(NOT ATLAS_BUILD_APPS)
        return()
    endif()

    set(target atlas_${name})
    add_executable(${target} ${ARG_SOURCES})

    foreach(dep IN LISTS ARG_DEPENDS)
        target_link_libraries(${target} PRIVATE ${dep})
    endforeach()

    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        FOLDER "apps")

    atlas_set_warnings(${target})
    atlas_set_common_compile_options(${target})
    atlas_apply_sanitizers(${target})
    atlas_enable_tidy(${target})
    atlas_link_tracy(${target})
endfunction()
