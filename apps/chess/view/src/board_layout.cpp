// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/chess/board_layout.hpp>

#include <cmath>

namespace atlas::chess {

math::Rect BoardLayout::square_rect(Square sq) const noexcept {
    const int file = file_of(sq);
    const int rank = rank_of(sq);
    const int column = flipped ? 7 - file : file;
    const int row = flipped ? rank : 7 - rank;
    return math::Rect{.position = {static_cast<float>(column), static_cast<float>(row)},
                      .size = {1.0F, 1.0F}};
}

std::optional<Square> BoardLayout::square_at(math::Vec2 world) const noexcept {
    if (!std::isfinite(world.x) || !std::isfinite(world.y) || world.x < 0.0F || world.y < 0.0F ||
        world.x >= kExtent || world.y >= kExtent) {
        return std::nullopt;
    }
    const int column = static_cast<int>(world.x);
    const int row = static_cast<int>(world.y);
    const int file = flipped ? 7 - column : column;
    const int rank = flipped ? row : 7 - row;
    return square(static_cast<std::uint8_t>(file), static_cast<std::uint8_t>(rank));
}

}  // namespace atlas::chess
