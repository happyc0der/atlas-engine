# SPDX-License-Identifier: GPL-3.0-or-later
#
# Applications are composition roots and acceptance harnesses, not games and not libraries.
#
# One recorded exception: apps/chess, a probe built to test whether a game can be written
# against the engine without a line of it reaching engine/ (ADR-0018). The rule still binds
# the lab and the sandbox, and a second game needs a second record.
add_subdirectory(common)
add_subdirectory(sandbox)
add_subdirectory(lab)
