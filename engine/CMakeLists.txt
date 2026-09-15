# SPDX-License-Identifier: GPL-3.0-or-later
#
# Engine modules, in dependency order. A module appears here in the milestone that first
# needs it, never in advance: an empty module is an abstraction without a call site.
add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(simulation)
