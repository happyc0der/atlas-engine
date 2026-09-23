// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/chess/position.hpp>
#include <atlas/chess/world.hpp>
#include <atlas/core/assert.hpp>

#include <format>
#include <memory>
#include <utility>

namespace atlas::chess {
namespace {

template <typename T> [[nodiscard]] T& table_as(sim::World& world, sim::TableId id) noexcept {
    auto* table = dynamic_cast<T*>(world.table(id));
    ATLAS_ASSERT_MSG(table != nullptr, "a chess table is missing or of the wrong type");
    return *table;
}

template <typename T>
[[nodiscard]] const T& table_as(const sim::World& world, sim::TableId id) noexcept {
    const auto* table = dynamic_cast<const T*>(world.table(id));
    ATLAS_ASSERT_MSG(table != nullptr, "a chess table is missing or of the wrong type");
    return *table;
}

}  // namespace

Result<ChessWorld> make_world() {
    ChessWorld chess;
    auto board = chess.world.add_table("chess.board", std::make_unique<BoardTable>());
    if (!board) {
        return std::unexpected(std::move(board).error().context("chess.board"));
    }
    auto state = chess.world.add_table("chess.state", std::make_unique<StateTable>());
    if (!state) {
        return std::unexpected(std::move(state).error().context("chess.state"));
    }
    auto history = chess.world.add_table("chess.history", std::make_unique<HistoryTable>());
    if (!history) {
        return std::unexpected(std::move(history).error().context("chess.history"));
    }
    auto result = chess.world.add_table("chess.result", std::make_unique<ResultTable>());
    if (!result) {
        return std::unexpected(std::move(result).error().context("chess.result"));
    }
    auto players = chess.world.add_table("chess.players", std::make_unique<PlayersTable>());
    if (!players) {
        return std::unexpected(std::move(players).error().context("chess.players"));
    }
    chess.ids = TableIds{.board = *board,
                         .state = *state,
                         .history = *history,
                         .result = *result,
                         .players = *players};
    set_start_position(chess.world, chess.ids);
    return chess;
}

void set_start_position(sim::World& world, const TableIds& ids) {
    Position::start().write_tables(board_table(world, ids), state_table(world, ids));
    history_table(world, ids).clear();
    result_table(world, ids).clear();
    // Players are deliberately left as they are: who holds which colour is the application's
    // to say, and resetting the board is not a reason to forget it.
}

BoardTable& board_table(sim::World& world, const TableIds& ids) noexcept {
    return table_as<BoardTable>(world, ids.board);
}

const BoardTable& board_table(const sim::World& world, const TableIds& ids) noexcept {
    return table_as<BoardTable>(world, ids.board);
}

StateTable& state_table(sim::World& world, const TableIds& ids) noexcept {
    return table_as<StateTable>(world, ids.state);
}

const StateTable& state_table(const sim::World& world, const TableIds& ids) noexcept {
    return table_as<StateTable>(world, ids.state);
}

HistoryTable& history_table(sim::World& world, const TableIds& ids) noexcept {
    return table_as<HistoryTable>(world, ids.history);
}

const HistoryTable& history_table(const sim::World& world, const TableIds& ids) noexcept {
    return table_as<HistoryTable>(world, ids.history);
}

ResultTable& result_table(sim::World& world, const TableIds& ids) noexcept {
    return table_as<ResultTable>(world, ids.result);
}

const ResultTable& result_table(const sim::World& world, const TableIds& ids) noexcept {
    return table_as<ResultTable>(world, ids.result);
}

PlayersTable& players_table(sim::World& world, const TableIds& ids) noexcept {
    return table_as<PlayersTable>(world, ids.players);
}

const PlayersTable& players_table(const sim::World& world, const TableIds& ids) noexcept {
    return table_as<PlayersTable>(world, ids.players);
}

Status validate_world(const sim::World& world, const TableIds& ids) {
    const auto& board = board_table(world, ids);
    const auto& state = state_table(world, ids);
    const auto& history = history_table(world, ids);

    std::array<int, 2> kings{0, 0};
    for (Square sq = 0; sq < kSquareCount; ++sq) {
        const Piece piece = board.at(sq);
        if (piece == Piece::None) {
            continue;
        }
        const auto colour = static_cast<std::size_t>(colour_of(piece));
        if (kind_of(piece) == PieceKind::King) {
            ++kings[colour];
        }
        if (kind_of(piece) == PieceKind::Pawn && (rank_of(sq) == 0 || rank_of(sq) == 7)) {
            return std::unexpected(
                Error(ErrorCode::MalformedData,
                      std::format("a pawn stands on square {}, which is a back rank", sq)));
        }
    }
    if (kings[0] != 1 || kings[1] != 1) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("the board holds {} white and {} black kings", kings[0], kings[1])));
    }

    // A castling right survives only while the king and that rook have never moved, which on
    // a board means they still stand on their squares. A right without them is a file that
    // would let a king castle through a rook that is not there.
    const auto right_needs = [&](std::uint8_t right, Square king, Square rook, Piece king_piece,
                                 Piece rook_piece) -> Status {
        if ((state.castling & right) == 0) {
            return ok();
        }
        if (board.at(king) != king_piece || board.at(rook) != rook_piece) {
            return std::unexpected(Error(
                ErrorCode::MalformedData,
                std::format("castling right {:#x} without its king and rook in place", right)));
        }
        return ok();
    };
    if (auto s = right_needs(kWhiteKingSide, square(4, 0), square(7, 0), Piece::WhiteKing,
                             Piece::WhiteRook);
        !s) {
        return s;
    }
    if (auto s = right_needs(kWhiteQueenSide, square(4, 0), square(0, 0), Piece::WhiteKing,
                             Piece::WhiteRook);
        !s) {
        return s;
    }
    if (auto s = right_needs(kBlackKingSide, square(4, 7), square(7, 7), Piece::BlackKing,
                             Piece::BlackRook);
        !s) {
        return s;
    }
    if (auto s = right_needs(kBlackQueenSide, square(4, 7), square(0, 7), Piece::BlackKing,
                             Piece::BlackRook);
        !s) {
        return s;
    }

    // An en passant file names a pawn that just moved two squares: it stands on the fourth
    // rank from the mover's side, and the squares it crossed are empty.
    if (state.en_passant_file != kNoEnPassant) {
        const bool white_to_move = state.side_to_move == Colour::White;
        const Square pawn = square(state.en_passant_file, white_to_move ? 4 : 3);
        const Square crossed = square(state.en_passant_file, white_to_move ? 5 : 2);
        const Square from = square(state.en_passant_file, white_to_move ? 6 : 1);
        const Piece expected = white_to_move ? Piece::BlackPawn : Piece::WhitePawn;
        if (board.at(pawn) != expected || board.at(crossed) != Piece::None ||
            board.at(from) != Piece::None) {
            return std::unexpected(
                Error(ErrorCode::MalformedData,
                      std::format("en passant file {} names no pawn that just moved two squares",
                                  state.en_passant_file)));
        }
    }

    // The history holds one key per position since the last irreversible move, including the
    // current one, so its length is the halfmove clock plus one — except in a world that has
    // never had a move applied, where the rules have not yet written the first key.
    if (!history.keys.empty() && history.keys.size() != state.halfmove_clock + std::size_t{1}) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("the history holds {} keys against a halfmove clock of {}",
                              history.keys.size(), state.halfmove_clock)));
    }
    return ok();
}

}  // namespace atlas::chess
