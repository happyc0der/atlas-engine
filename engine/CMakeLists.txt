# SPDX-License-Identifier: GPL-3.0-or-later
#
# Engine modules, in dependency order. A module appears here in the milestone that first
# needs it, never in advance: an empty module is an abstraction without a call site.
add_subdirectory(core)
add_subdirectory(math)
add_subdirectory(tasks)
add_subdirectory(platform)
add_subdirectory(assets)
add_subdirectory(rhi)
add_subdirectory(renderer)
add_subdirectory(scene)
add_subdirectory(edit)
add_subdirectory(simulation)
add_subdirectory(tools)
