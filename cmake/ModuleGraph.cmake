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
    audio
    scene
    edit
    animation
    simulation
    net
    script
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
# audio: the output device, the mixer and the voices. Depends on platform because the platform
# owns SDL's lifetime and brings the audio subsystem up; this module opens a device on it, the
# way rhi opens a graphics device on a window it did not create. It does not depend on renderer,
# scene or simulation, and must not: a sound is triggered by whoever observes state, never by
# state itself, and that separation is what keeps audio out of every hash.
# The assets edge arrived with the clip asset type: the device is the finaliser for decoded
# audio, exactly as the texture cache is for decoded pixels, and a finaliser has to be able to
# name the registry it takes from.
set(ATLAS_MODULE_DEPS_audio             "core;assets;platform" CACHE INTERNAL "")
set(ATLAS_MODULE_DEPS_scene             "core;math;assets;rhi" CACHE INTERNAL "")
# edit: undoable commands over the scene and the history that applies them. Between scene and
# tools so the command layer is testable without a UI library, and so a panel can be handed a
# History without ever seeing a mutable Scene.
set(ATLAS_MODULE_DEPS_edit              "core;scene" CACHE INTERNAL "")
# animation: clip evaluation and the derived pose it writes. Listed before it exists, as this
# file's own policy asks, so that the shape is reviewable rather than arriving with the code.
# Depends on scene because the pose is a scene component, and on assets because a clip is an
# asset like any other. It must never depend on renderer or simulation: animation is
# presentation, it is hashed nowhere, and what draws the result is not its concern.
set(ATLAS_MODULE_DEPS_animation         "core;math;assets;scene" CACHE INTERNAL "")
set(ATLAS_MODULE_DEPS_simulation        "core;tasks" CACHE INTERNAL "")

# net: deterministic lockstep over the command queue, and nothing else (ADR-0014). It holds
# what is genuinely about peers — a bounded inbox, the message codec, an in-memory link, and a
# session — while the turn gate and the command-source interface live in simulation, because a
# mod is also a command source and must not depend on networking for an interface about the
# command queue. No transport edge: there is no socket in this milestone by decision, and
# adding one is a separate change with its own ADR. It must never depend on renderer, scene or
# platform: a peer exchanges commands and hashes, and nothing it does is presentation.
set(ATLAS_MODULE_DEPS_net               "core;simulation" CACHE INTERNAL "")

# script: the sandbox untrusted mods run in (ADR-0015), and nothing else. It depends on
# simulation for `CommandSource` and the command queue a mod reaches state through, and on
# assets for `VirtualPath`, because a mod is untrusted input loaded from a mounted root exactly
# as a save file is. No `net` edge, deliberately: a mod is a command source and the interface
# for that lives in simulation, so a sandbox must not depend on networking to produce a
# command. It must never depend on scene, renderer or platform — a mod submits commands and
# reads bytes, and none of what it does is presentation.
set(ATLAS_MODULE_DEPS_script            "core;simulation;assets" CACHE INTERNAL "")
set(ATLAS_MODULE_DEPS_runtime           "core;math;platform;rhi;renderer;assets;scene;simulation" CACHE INTERNAL "")
set(ATLAS_MODULE_DEPS_rhi_internal      "core;platform;rhi" CACHE INTERNAL "")
set(ATLAS_MODULE_DEPS_tools             "core;math;platform;platform_internal;rhi;rhi_internal;renderer;assets;scene;edit;simulation" CACHE INTERNAL "")

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
