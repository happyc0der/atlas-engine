# SPDX-License-Identifier: GPL-3.0-or-later
# Module dependency allow-list.
#
# This table is the single source of truth for which Atlas module may depend on which.
# atlas_add_module() rejects any DEPENDS entry that is not listed here, at configure time.
# tools/check_module_deps.py reads the same table to check includes and to detect cycles.
#
# Adding an edge means editing this file, which means writing down a reason. That is the
# point: a boundary that can be crossed silently is not a boundary.
#
# Modules not yet created are listed anyway, so the intended shape is visible and so the
# graph can be reviewed as a whole rather than one milestone at a time.

# Format: ATLAS_MODULE_DEPS_<module> is the set of Atlas modules <module> may depend on.

set(ATLAS_MODULES
    core
    math
    platform
    platform_internal
    rhi_internal
    tasks
    rhi
    renderer
    assets
    scene
    simulation
    runtime
    tools
    CACHE INTERNAL "All known Atlas module names")

set(ATLAS_MODULE_DEPS_core              "" CACHE INTERNAL "")
set(ATLAS_MODULE_DEPS_math              "core" CACHE INTERNAL "")
set(ATLAS_MODULE_DEPS_platform          "core" CACHE INTERNAL "")
set(ATLAS_MODULE_DEPS_platform_internal "core;platform" CACHE INTERNAL "")
set(ATLAS_MODULE_DEPS_tasks             "core" CACHE INTERNAL "")
set(ATLAS_MODULE_DEPS_rhi               "core;platform;platform_internal" CACHE INTERNAL "")
set(ATLAS_MODULE_DEPS_renderer          "core;math;rhi;assets" CACHE INTERNAL "")
set(ATLAS_MODULE_DEPS_assets            "core;platform" CACHE INTERNAL "")
set(ATLAS_MODULE_DEPS_scene             "core;math" CACHE INTERNAL "")
set(ATLAS_MODULE_DEPS_simulation        "core;tasks" CACHE INTERNAL "")
set(ATLAS_MODULE_DEPS_runtime           "core;math;platform;rhi;renderer;assets;scene;simulation" CACHE INTERNAL "")
set(ATLAS_MODULE_DEPS_rhi_internal      "core;platform;rhi" CACHE INTERNAL "")
set(ATLAS_MODULE_DEPS_tools             "core;math;platform;platform_internal;rhi;rhi_internal;renderer;assets;scene;simulation;runtime" CACHE INTERNAL "")

# Third-party libraries permitted in a module's PUBLIC headers. Everything else must be a
# private implementation detail. Each entry needs an ADR.
#   - Tracy in core/profile.hpp: the zone macros are the API, and they compile to nothing
#     unless ATLAS_PROFILE is on. ADR pending; see docs/PERFORMANCE.md.
#   - EnTT in scene: ADR-0004.
# platform_internal is an interface target, not a module with sources: it exposes the
# native window handle to the renderer and to nothing else. Listed here so that the
# boundary script treats a use of it outside rhi as the violation it would be.
set(ATLAS_PUBLIC_THIRDPARTY_core  "tracy" CACHE INTERNAL "")
set(ATLAS_PUBLIC_THIRDPARTY_scene "entt"  CACHE INTERNAL "")

# Validate the table itself: every dependency must name a known module, and the graph must
# be acyclic. A typo here would otherwise silently widen a boundary.
function(_atlas_validate_module_graph)
    foreach(module IN LISTS ATLAS_MODULES)
        foreach(dep IN LISTS ATLAS_MODULE_DEPS_${module})
            if(NOT dep IN_LIST ATLAS_MODULES)
                message(FATAL_ERROR
                    "ModuleGraph.cmake: module '${module}' declares unknown dependency '${dep}'")
            endif()
            if(dep STREQUAL module)
                message(FATAL_ERROR "ModuleGraph.cmake: module '${module}' depends on itself")
            endif()
        endforeach()
    endforeach()

    # Depth-first cycle detection. CMake tolerates static-library cycles at link time, so
    # nothing else would catch this.
    foreach(start IN LISTS ATLAS_MODULES)
        set(_stack "${start}")
        set(_seen "")
        while(_stack)
            list(POP_BACK _stack current)
            if(current IN_LIST _seen)
                continue()
            endif()
            list(APPEND _seen "${current}")
            foreach(dep IN LISTS ATLAS_MODULE_DEPS_${current})
                if(dep STREQUAL start)
                    message(FATAL_ERROR
                        "ModuleGraph.cmake: dependency cycle involving '${start}' via '${current}'")
                endif()
                list(APPEND _stack "${dep}")
            endforeach()
        endwhile()
    endforeach()
endfunction()

_atlas_validate_module_graph()
