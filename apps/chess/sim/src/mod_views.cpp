// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/chess/mod_views.hpp>
#include <atlas/chess/world.hpp>

#include <cstddef>

namespace atlas::chess {
namespace {

void write_u16(PositionView& out, std::size_t at, std::uint16_t value) {
    const auto bits = static_cast<unsigned>(value);
    out[at] = static_cast<std::uint8_t>(bits & 0xFFU);
    out[at + 1] = static_cast<std::uint8_t>((bits >> 8U) & 0xFFU);
}

}  // namespace

PositionView position_view(const sim::World& world, const TableIds& ids) {
    const BoardTable& board = board_table(world, ids);
    const StateTable& state = state_table(world, ids);
    const ResultTable& result = result_table(world, ids);

    PositionView out{};
    for (std::size_t sq = 0; sq < board.squares.size(); ++sq) {
        out[ATLAS_CHESS_VIEW_SQUARES + sq] = board.squares[sq];
    }
    out[ATLAS_CHESS_VIEW_SIDE] = static_cast<std::uint8_t>(state.side_to_move);
    out[ATLAS_CHESS_VIEW_CASTLING] = state.castling;
    out[ATLAS_CHESS_VIEW_EN_PASSANT] = state.en_passant_file;
    write_u16(out, ATLAS_CHESS_VIEW_HALFMOVE, state.halfmove_clock);
    write_u16(out, ATLAS_CHESS_VIEW_FULLMOVE, state.fullmove_number);
    out[ATLAS_CHESS_VIEW_OUTCOME] = static_cast<std::uint8_t>(result.outcome);
    return out;
}

}  // namespace atlas::chess
