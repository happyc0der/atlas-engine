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
