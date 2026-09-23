// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The one command, and the whole of chess behind it.
///
/// `chess.move` is three bytes: from, to, promotion. Its handler reconstructs the position
/// from the tables, asks whether the move is legal for the side to move, checks that the
/// source that sent it holds that colour, and **declines** if any of that is false
/// (ADR-0019). Otherwise it makes the move, rewrites the tables, extends or resets the
/// repetition history, and — if the position is now final — writes the result. A move after
/// the result is set is declined too.
///
/// Every rule is applied inside the tick, on hashed state, identically on every peer. The
/// application may predict legality before submitting so a person is not made to wait for the
/// decline; the check here is the authoritative one, and the only one a remote peer's client
/// cannot skip.
///
/// Draws by the fifty-move rule and by threefold repetition are applied automatically, at the
/// move that reaches them, rather than claimed; the claimable form is recorded as deferred.

#include <atlas/chess/position.hpp>
#include <atlas/chess/tables.hpp>
#include <atlas/core/result.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/world.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace atlas::chess {

inline const sim::CommandType kMoveCommand = sim::command_type("chess.move");
inline constexpr std::size_t kMoveCommandBytes = 3;

/// A move as a payload, and back. `decode_move` checks the squares are on the board, the
/// promotion byte names a piece a pawn can become or nothing, and the move goes somewhere.
[[nodiscard]] std::array<std::byte, kMoveCommandBytes> encode_move(Move move) noexcept;
[[nodiscard]] Result<Move> decode_move(std::span<const std::byte> payload);

/// Put a position into a world's tables as the start of a game: the board and state as given,
/// the history holding the position's own key, the result cleared. The players are left as
/// they are.
void set_position(sim::World& world, const TableIds& ids, const Position& position);

/// Apply a move to the tables as the rules would for a command from `source`.
///
/// This is the handler's body, exposed so a test can drive the rules without a kernel. An
/// error is a decline: the game is over, it is not this source's turn, or the move is not
/// legal in this position. A decline has changed nothing.
[[nodiscard]] Status make_move(sim::World& world, const TableIds& ids, Move move,
                               sim::SourceId source);

/// Register `chess.move` against the tables.
[[nodiscard]] Status register_chess_commands(sim::CommandQueue& commands, const TableIds& ids);

/// The legal moves in a world's position, for an application that wants to show them.
void legal_moves(const sim::World& world, const TableIds& ids, MoveList& out);

}  // namespace atlas::chess
