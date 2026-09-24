// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/chess/board_quads.hpp>

namespace atlas::chess {
namespace {

constexpr rhi::Colour kLightSquare{.r = 0.93F, .g = 0.86F, .b = 0.73F, .a = 1.0F};
constexpr rhi::Colour kDarkSquare{.r = 0.70F, .g = 0.53F, .b = 0.39F, .a = 1.0F};
constexpr rhi::Colour kLastMove{.r = 0.96F, .g = 0.84F, .b = 0.35F, .a = 0.45F};
constexpr rhi::Colour kSelected{.r = 0.35F, .g = 0.62F, .b = 0.92F, .a = 0.55F};
constexpr rhi::Colour kTarget{.r = 0.12F, .g = 0.14F, .b = 0.18F, .a = 0.45F};
constexpr rhi::Colour kCapture{.r = 0.85F, .g = 0.25F, .b = 0.20F, .a = 0.45F};
constexpr rhi::Colour kCheck{.r = 0.90F, .g = 0.15F, .b = 0.12F, .a = 0.60F};
constexpr rhi::Colour kOpaque{.r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F};

[[nodiscard]] renderer::Quad tinted(const math::Rect& bounds, rhi::Colour colour) {
    return renderer::Quad{.bounds = bounds, .uv = sheet_cell(kSolidCell, 0), .colour = colour};
}

}  // namespace

math::Rect sheet_cell(int column, int row) noexcept {
    constexpr float kWidth = 1.0F / static_cast<float>(kSheetColumns);
    constexpr float kHeight = 1.0F / static_cast<float>(kSheetRows);
    return math::Rect{
        .position = {static_cast<float>(column) * kWidth, static_cast<float>(row) * kHeight},
        .size = {kWidth, kHeight}};
}

math::Rect piece_cell(Piece piece) noexcept {
    const int column = static_cast<int>(kind_of(piece)) - 1;
    const int row = colour_of(piece) == Colour::White ? 0 : 1;
    return sheet_cell(column, row);
}

void build_board_quads(const Position& position, const BoardMarks& marks, const BoardLayout& layout,
                       std::vector<renderer::Quad>& out) {
    // Squares. a1 is dark, which is the one fact about a chessboard everybody can check.
    for (Square sq = 0; sq < kSquareCount; ++sq) {
        const bool dark = (file_of(sq) + rank_of(sq)) % 2 == 0;
        out.push_back(tinted(layout.square_rect(sq), dark ? kDarkSquare : kLightSquare));
    }

    if (marks.last_move.has_value()) {
        out.push_back(tinted(layout.square_rect(marks.last_move->from), kLastMove));
        out.push_back(tinted(layout.square_rect(marks.last_move->to), kLastMove));
    }
    if (marks.in_check.has_value()) {
        out.push_back(tinted(layout.square_rect(*marks.in_check), kCheck));
    }
    if (marks.selected.has_value()) {
        out.push_back(tinted(layout.square_rect(*marks.selected), kSelected));
    }
    for (const Move& move : marks.targets) {
        // Four promotions share a target square; mark it once.
        if (move.promotion != PieceKind::None && move.promotion != PieceKind::Queen) {
            continue;
        }
        if (position.at(move.to) == Piece::None) {
            out.push_back(renderer::Quad{.bounds = layout.square_rect(move.to),
                                         .uv = sheet_cell(kRingCell, 0),
                                         .colour = kTarget});
        } else {
            out.push_back(tinted(layout.square_rect(move.to), kCapture));
        }
    }

    for (Square sq = 0; sq < kSquareCount; ++sq) {
        const Piece piece = position.at(sq);
        if (piece == Piece::None) {
            continue;
        }
        out.push_back(renderer::Quad{
            .bounds = layout.square_rect(sq), .uv = piece_cell(piece), .colour = kOpaque});
    }
}

}  // namespace atlas::chess
