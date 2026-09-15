# SPDX-License-Identifier: GPL-3.0-or-later
# Tracy profiler client.
#
# Compiled out entirely unless ATLAS_PROFILE is ON, which is what "from milestone zero,
# compiled out in normal builds" means in practice: the zone macros in
# atlas/core/profile.hpp expand to nothing, and no Tracy symbol reaches the binary.
#
# The client and the Tracy GUI must be the same version; the wire protocol changes between
# releases. The pinned pair is recorded in docs/DEPENDENCIES.md.

function(atlas_configure_tracy)
    if(NOT ATLAS_PROFILE)
        return()
    endif()

    find_package(Tracy CONFIG REQUIRED)
    message(STATUS "Atlas: Tracy profiling enabled (client must match the Tracy GUI version)")
endfunction()

# Applied to every first-party target so that the macros resolve consistently.
function(atlas_link_tracy target)
    if(ATLAS_PROFILE)
        target_compile_definitions(${target} PUBLIC ATLAS_TRACY_ENABLED=1)
        target_link_libraries(${target} PUBLIC Tracy::TracyClient)
    else()
        target_compile_definitions(${target} PUBLIC ATLAS_TRACY_ENABLED=0)
    endif()
endfunction()
