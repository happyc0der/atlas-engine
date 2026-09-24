// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Where each square is in the world, and which square a world point is on.
///
/// One world unit is one square, and the board occupies [0, 8) on both axes. The world is
/// screen-like — y grows downwards (`math::OrthoCamera`) — so with white at the bottom the
/// eighth rank is at y = 0 and the first at y = 7. A flipped layout puts black at the bottom,
/// which is what the second player of a networked game wants to see.
///
/// Integer arithmetic over squares and floating point only at the edge, where a world point
/// comes in from a click. Nothing here reaches simulation state.

#include <atlas/chess/piece.hpp>
#include <atlas/math/vector.hpp>

#include <optional>

namespace atlas::chess {

struct BoardLayout {
    /// Black at the bottom rather than white.
    bool flipped = false;

    /// The world rectangle a square covers.
    [[nodiscard]] math::Rect square_rect(Square sq) const noexcept;

    /// The square under a world point, or nothing off the board. Each square is half-open, so
    /// a point on the line between two squares belongs to exactly one of them.
    [[nodiscard]] std::optional<Square> square_at(math::Vec2 world) const noexcept;

    /// The middle of the board, where a camera looking at it should be centred.
    [[nodiscard]] static constexpr math::Vec2 centre() noexcept { return {.x = 4.0F, .y = 4.0F}; }

    /// World units the board spans on each side.
    static constexpr float kExtent = 8.0F;
};

}  // namespace atlas::chess
