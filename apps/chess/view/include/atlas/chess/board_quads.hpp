// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// A position, and what a person has selected in it, as quads for `renderer::QuadBatch`.
///
/// Pure: it writes quads into a caller's vector and touches no device, so what the board looks
/// like is testable without one. Every quad samples the piece sheet `tools/gen_textures.py`
/// writes — `textures/chess_pieces.png`, eight cells a row, white pieces on the top row and black
/// below in `PieceKind` order, then a solid cell the squares are tinted from and a ring that
/// marks a move to an empty square — so one texture and one draw call cover the whole board.

#include <atlas/chess/board_layout.hpp>
#include <atlas/chess/position.hpp>
#include <atlas/renderer/quad_batch.hpp>

#include <optional>
#include <span>
#include <vector>

namespace atlas::chess {

/// The sheet's layout, in cells.
inline constexpr int kSheetColumns = 8;
inline constexpr int kSheetRows = 2;
inline constexpr int kSolidCell = 6;
inline constexpr int kRingCell = 7;

/// The normalised rectangle of one cell of the sheet.
[[nodiscard]] math::Rect sheet_cell(int column, int row) noexcept;

/// The cell a piece is drawn from. Undefined for `Piece::None`, which is never drawn.
[[nodiscard]] math::Rect piece_cell(Piece piece) noexcept;

/// What the person looking at the board has chosen or should notice.
struct BoardMarks {
    /// The square whose piece is picked up.
    std::optional<Square> selected;
    /// The legal moves of that piece: an empty target gets a ring, an occupied one a tint.
    std::span<const Move> targets;
    /// The last move made, so the other player can see what changed.
    std::optional<Move> last_move;
    /// A king in check, tinted so it cannot be missed.
    std::optional<Square> in_check;
};

/// Append the board's quads to `out`, in draw order: squares, marks, then pieces. Clears
/// nothing, so a caller can reuse one vector across frames without allocating after the first.
void build_board_quads(const Position& position, const BoardMarks& marks, const BoardLayout& layout,
                       std::vector<renderer::Quad>& out);

}  // namespace atlas::chess
