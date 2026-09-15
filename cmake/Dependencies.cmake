# SPDX-License-Identifier: GPL-3.0-or-later
# Dependency resolution guards.
#
# vcpkg supplies every third-party library. The development machine also has Homebrew
# copies of some of them (SDL3, for one), and silently linking a different version than the
# manifest pins would produce confusing, irreproducible failures.

# Verify that a package resolved to something inside vcpkg_installed rather than to a
# system copy. Call after find_package for anything that has a plausible system version.
function(atlas_assert_from_vcpkg package_name resolved_path)
    if(NOT resolved_path)
        message(FATAL_ERROR "Atlas: ${package_name} did not resolve to any path.")
    endif()

    file(REAL_PATH "${resolved_path}" _resolved)
    string(FIND "${_resolved}" "vcpkg_installed" _idx)
    if(_idx EQUAL -1)
        message(FATAL_ERROR
            "Atlas: ${package_name} resolved to '${_resolved}', which is outside "
            "vcpkg_installed. This is usually a system or Homebrew copy shadowing the "
            "pinned version. Builds must use the version pinned in vcpkg.json.")
    endif()
endfunction()

# The SDL3 target to link against, as one name that works on every triplet.
#
# SDL3 exports a different target depending on how it was built, and vcpkg builds it
# differently per platform: the default triplets on macOS and Linux are static and produce
# `SDL3::SDL3-static`, while `x64-windows` is dynamic and produces `SDL3::SDL3-shared`. A
# build file naming either one directly works on some platforms and fails to configure on the
# rest, which is exactly what happened the first time Windows was ever built.
#
# So the choice is made once, here, and every module links `atlas::sdl3`. Adding a platform
# means changing this function and nothing else.
#
# Call after find_package(SDL3 CONFIG REQUIRED). Safe to call repeatedly; the target is
# created once.
function(atlas_define_sdl3_target)
    if(TARGET atlas_sdl3)
        return()
    endif()

    # Ordered by preference rather than by likelihood. A static link is what Atlas wants
    # wherever it is available: it keeps the runtime dependency set to system libraries only,
    # which matters for a tool that gets copied between machines.
    set(_candidates SDL3::SDL3-static SDL3::SDL3-shared SDL3::SDL3)

    set(_chosen "")
    foreach(_candidate IN LISTS _candidates)
        if(TARGET ${_candidate})
            set(_chosen "${_candidate}")
            break()
        endif()
    endforeach()

    if(NOT _chosen)
        message(FATAL_ERROR
            "Atlas: SDL3 was found but exports none of ${_candidates}. The vcpkg port's "
            "target names have changed; update atlas_define_sdl3_target in "
            "cmake/Dependencies.cmake.")
    endif()

    add_library(atlas_sdl3 INTERFACE)
    add_library(atlas::sdl3 ALIAS atlas_sdl3)
    target_link_libraries(atlas_sdl3 INTERFACE ${_chosen})

    message(STATUS "Atlas: SDL3 links as ${_chosen}")
endfunction()
