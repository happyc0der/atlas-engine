// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The chess position as the bytes a mod reads (ADR-0023 D7).
///
/// Built from the world at the tick boundary, like every view (ADR-0015 D5), and laid out by
/// `mod_view.h`, which the mod includes too. Written a byte at a time and little-endian, never by
/// copying a structure, so the bytes do not depend on this compiler's padding or byte order —
/// the lab's `mod_views.cpp` is the precedent.
///
/// Thread affinity: none; pure functions of a const world.

#include <atlas/chess/mod_view.h>
#include <atlas/chess/tables.hpp>
#include <atlas/simulation/world.hpp>

#include <array>
#include <cstdint>

namespace atlas::chess {

using PositionView = std::array<std::uint8_t, ATLAS_CHESS_VIEW_POSITION_BYTES>;

/// Which side or sides a mod plays, as the seat view's one byte.
enum class Seat : std::uint8_t {
    White = ATLAS_CHESS_SEAT_WHITE,
    Black = ATLAS_CHESS_SEAT_BLACK,
    Both = ATLAS_CHESS_SEAT_BOTH,
};

/// The position view: squares, side, castling, en passant, the clocks and the outcome.
[[nodiscard]] PositionView position_view(const sim::World& world, const TableIds& ids);

}  // namespace atlas::chess
