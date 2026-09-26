# SPDX-License-Identifier: GPL-3.0-or-later
#
# Applications are composition roots and acceptance harnesses, not games and not libraries.
#
# Chess was the one recorded exception, a probe written here from M18 to M26 (ADR-0018). Since
# M27 it is its own repository, happyc0der/atlas-chess, built against an installed Atlas and
# nothing else (ADR-0024). No game is in this repository.
add_subdirectory(common)
add_subdirectory(sandbox)
add_subdirectory(lab)
