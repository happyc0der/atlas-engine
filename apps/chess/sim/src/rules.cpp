// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/chess/fen.hpp>
#include <atlas/chess/rules.hpp>
#include <atlas/chess/world.hpp>

#include <algorithm>
#include <format>
#include <utility>

namespace atlas::chess {
namespace {

/// The number of times the fifty-move rule's count reaches before the game draws itself:
/// fifty moves by each side is a hundred plies.
constexpr std::uint16_t kFiftyMovePlies = 100;
/// The repetition that draws the game automatically.
constexpr std::size_t kRepetitionsToDraw = 3;

[[nodiscard]] bool promotion_code_is_valid(std::uint8_t code) noexcept {
    return code == 0 || (code >= static_cast<std::uint8_t>(PieceKind::Knight) &&
                         code <= static_cast<std::uint8_t>(PieceKind::Queen));
}

}  // namespace

std::array<std::byte, kMoveCommandBytes> encode_move(Move move) noexcept {
    return {static_cast<std::byte>(move.from), static_cast<std::byte>(move.to),
            static_cast<std::byte>(move.promotion)};
}

Result<Move> decode_move(std::span<const std::byte> payload) {
    if (payload.size() != kMoveCommandBytes) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("chess.move needs {} bytes, got {}",
                                                        kMoveCommandBytes, payload.size())));
    }
    const auto from = std::to_integer<std::uint8_t>(payload[0]);
    const auto to = std::to_integer<std::uint8_t>(payload[1]);
    const auto promotion = std::to_integer<std::uint8_t>(payload[2]);
    if (from >= kSquareCount || to >= kSquareCount) {
        return std::unexpected(
            Error(ErrorCode::OutOfRange,
                  std::format("chess.move names squares {} and {}; the board has 64", from, to)));
    }
    if (from == to) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "chess.move goes nowhere: from and to are equal"));
    }
    if (!promotion_code_is_valid(promotion)) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("chess.move promotion code {} is not a piece a pawn can become",
                              promotion)));
    }
    return Move{.from = from, .to = to, .promotion = static_cast<PieceKind>(promotion)};
}

void set_position(sim::World& world, const TableIds& ids, const Position& position) {
    position.write_tables(board_table(world, ids), state_table(world, ids));
    auto& history = history_table(world, ids);
    history.clear();
    history.keys.push_back(position.key());
    result_table(world, ids).clear();
}

Status make_move(sim::World& world, const TableIds& ids, Move move, sim::SourceId source) {
    // Decided before anything is written: every check below reads, and the writes begin only
    // once all of them have passed. That is the whole of ADR-0019's contract for a handler.
    auto& result = result_table(world, ids);
    if (result.outcome != Outcome::Ongoing) {
        return std::unexpected(Error(
            ErrorCode::Unavailable, std::format("the game is over ({}), so {} is declined",
                                                static_cast<int>(result.reason), to_string(move))));
    }

    auto& board = board_table(world, ids);
    auto& state = state_table(world, ids);
    const Position position = Position::from_tables(board, state);

    const auto& players = players_table(world, ids);
    if (players.holder(position.side_to_move) != source) {
        return std::unexpected(
            Error(ErrorCode::PermissionDenied,
                  std::format("source {} moved for {}, whose moves come from source {}",
                              static_cast<std::uint32_t>(source),
                              position.side_to_move == Colour::White ? "white" : "black",
                              static_cast<std::uint32_t>(players.holder(position.side_to_move)))));
    }

    if (!position.is_legal(move)) {
        const Piece piece = position.at(move.from);
        const char* why = "it is not a legal move in this position";
        if (piece == Piece::None) {
            why = "there is no piece there";
        } else if (colour_of(piece) != position.side_to_move) {
            why = "that piece is not the mover's";
        }
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     std::format("{} is declined: {}", to_string(move), why)));
    }

    // Everything has passed; from here the tables are rewritten.
    const Position next = position.after(move);
    next.write_tables(board, state);

    // The history holds the keys since the last irreversible move, including the current
    // position; a move that reset the clock starts it afresh.
    auto& history = history_table(world, ids);
    if (next.halfmove_clock == 0) {
        history.keys.clear();
    }
    history.keys.push_back(next.key());

    // The endings, in an order that matters: a move that mates wins even if it is also the
    // hundredth quiet ply, so mate and stalemate are asked first.
    MoveList replies;
    next.generate_legal(replies);
    if (replies.empty()) {
        if (next.in_check(next.side_to_move)) {
            result.outcome =
                position.side_to_move == Colour::White ? Outcome::WhiteWins : Outcome::BlackWins;
            result.reason = Reason::Checkmate;
        } else {
            result.outcome = Outcome::Draw;
            result.reason = Reason::Stalemate;
        }
        return ok();
    }
    if (next.insufficient_material()) {
        result.outcome = Outcome::Draw;
        result.reason = Reason::InsufficientMaterial;
        return ok();
    }
    if (next.halfmove_clock >= kFiftyMovePlies) {
        result.outcome = Outcome::Draw;
        result.reason = Reason::FiftyMoves;
        return ok();
    }
    const auto repetitions =
        static_cast<std::size_t>(std::count(history.keys.begin(), history.keys.end(), next.key()));
    if (repetitions >= kRepetitionsToDraw) {
        result.outcome = Outcome::Draw;
        result.reason = Reason::Threefold;
    }
    return ok();
}

Status register_chess_commands(sim::CommandQueue& commands, const TableIds& ids) {
    sim::CommandHandler handler;
    handler.validate = [](std::span<const std::byte> payload) -> Status {
        auto decoded = decode_move(payload);
        if (!decoded) {
            return std::unexpected(std::move(decoded).error());
        }
        return ok();
    };
    handler.apply = [ids](sim::World& world, const sim::ApplyContext& context,
                          std::span<const std::byte> payload) -> Status {
        auto decoded = decode_move(payload);
        if (!decoded) {
            // The queue re-validated an instant ago with the same bytes; this cannot fail on
            // one thread, and returning the reason is cheaper than trusting it.
            return std::unexpected(std::move(decoded).error());
        }
        return make_move(world, ids, *decoded, context.source);
    };
    return commands.register_handler(kMoveCommand, std::move(handler));
}

void legal_moves(const sim::World& world, const TableIds& ids, MoveList& out) {
    if (result_table(world, ids).outcome != Outcome::Ongoing) {
        return;
    }
    Position::from_tables(board_table(world, ids), state_table(world, ids)).generate_legal(out);
}

}  // namespace atlas::chess
