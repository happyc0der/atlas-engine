// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Notation: Forsyth–Edwards for a position, coordinates for a move. Both are how a test names
/// a position and how the application will take one from a command line. Neither is text a
/// person sees on the interface, so neither goes through a string table; a FEN is an
/// identifier, like a hash.
///
/// A parser here treats its input as untrusted and bounded, like every other reader in the
/// tree: a length cap before anything is looked at, then every field checked.

#include <atlas/chess/position.hpp>
#include <atlas/core/result.hpp>

#include <string>
#include <string_view>

namespace atlas::chess {

/// Longest FEN this parser will look at. A legal one is under ninety characters.
inline constexpr std::size_t kMaxFenLength = 128;

/// Parse a FEN. The clocks are optional and default to 0 and 1. The en passant square is
/// recorded only when an enemy pawn can use it, matching `Position::after`. What is checked is
/// the grammar and the ranges; whether the position is one chess can be played from is
/// `validate_world`'s question, asked after the position is in the tables.
[[nodiscard]] Result<Position> parse_fen(std::string_view fen);

/// The FEN of a position, all six fields.
[[nodiscard]] std::string to_fen(const Position& position);

/// Parse "e2e4" or "e7e8q": from, to, and an optional promotion letter (q, r, b, n).
[[nodiscard]] Result<Move> parse_move(std::string_view text);

/// "e2e4" or "e7e8q".
[[nodiscard]] std::string to_string(Move move);

/// "e4" for a square index.
[[nodiscard]] std::string square_name(Square sq);

}  // namespace atlas::chess
