// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// A position as a value, and the rules of movement over it.
///
/// The tables are where a position lives; this is where it is reasoned about. A `Position` is
/// a plain copyable struct — the board and the four facts a move needs — so that legality is
/// checked by *copy-make*: make the move on a copy and ask whether the mover's king is
/// attacked. Slow and obvious is the target. The board is sixty-four squares and a game is a
/// hundred plies; an engine that searched would want incremental update, and this library does
/// not search.
///
/// Nothing here allocates. A move list is a fixed array, because no chess position has more
/// than 218 legal moves and the bound is a fact rather than a guess.
///
/// Everything is integer and every function is a pure function of its arguments, which is what
/// lets two peers make the same decision about the same move at the same tick.

#include <atlas/chess/piece.hpp>
#include <atlas/chess/tables.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace atlas::chess {

/// One move: from, to, and what a pawn becomes if it reaches the last rank. Castling is the
/// king moving two files; en passant is a pawn moving to the en passant square. Neither needs
/// a flag, because the position it is made in says which it is.
struct Move {
    Square from = 0;
    Square to = 0;
    PieceKind promotion = PieceKind::None;

    [[nodiscard]] constexpr bool operator==(const Move&) const noexcept = default;
};

/// The most legal moves any position has is 218; a pseudo-legal list is a little longer.
class MoveList {
  public:
    static constexpr std::size_t kCapacity = 256;

    void push(Move move) noexcept {
        if (m_count < kCapacity) {
            m_moves[m_count++] = move;
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_count; }

    [[nodiscard]] bool empty() const noexcept { return m_count == 0; }

    [[nodiscard]] const Move& operator[](std::size_t index) const noexcept {
        return m_moves[index];
    }

    [[nodiscard]] const Move* begin() const noexcept { return m_moves.data(); }

    [[nodiscard]] const Move* end() const noexcept { return m_moves.data() + m_count; }

    [[nodiscard]] bool contains(Move move) const noexcept;

  private:
    std::array<Move, kCapacity> m_moves{};
    std::size_t m_count = 0;
};

struct Position {
    std::array<std::uint8_t, kSquareCount> squares{};
    Colour side_to_move = Colour::White;
    std::uint8_t castling = kAllCastling;
    std::uint8_t en_passant_file = kNoEnPassant;
    std::uint16_t halfmove_clock = 0;
    std::uint16_t fullmove_number = 1;

    [[nodiscard]] constexpr bool operator==(const Position&) const noexcept = default;

    /// The position the board and state tables describe.
    [[nodiscard]] static Position from_tables(const BoardTable& board, const StateTable& state);
    /// Write this position into the tables, replacing what they hold.
    void write_tables(BoardTable& board, StateTable& state) const;
    /// The standard starting position.
    [[nodiscard]] static Position start();

    [[nodiscard]] Piece at(Square sq) const noexcept { return static_cast<Piece>(squares[sq]); }

    void put(Square sq, Piece piece) noexcept { squares[sq] = static_cast<std::uint8_t>(piece); }

    /// Where a colour's king stands. A position without one is not a position; the caller
    /// validated the world before asking.
    [[nodiscard]] Square king_square(Colour colour) const noexcept;
    /// Whether any piece of `by` attacks `sq`. Pins are ignored, which is the definition.
    [[nodiscard]] bool is_attacked(Square sq, Colour by) const noexcept;
    /// Whether `colour`'s king is attacked.
    [[nodiscard]] bool in_check(Colour colour) const noexcept;

    /// Every move a piece could make by its own rules, without asking whether the king is left
    /// in check. Castling checks its own squares here, because "not through check" is part of
    /// the castling rule rather than of legality in general.
    void generate_pseudo_legal(MoveList& out) const;
    /// The pseudo-legal moves that leave the mover's king unattacked.
    void generate_legal(MoveList& out) const;
    /// Whether `move` is in the legal list of this position.
    [[nodiscard]] bool is_legal(Move move) const;

    /// The position after a pseudo-legal move: the piece moved, a capture removed, a castling
    /// rook moved, an en passant pawn removed, a promotion made, the rights, the clocks, the
    /// en passant file and the side to move all updated. Given a move that is not pseudo-legal
    /// the result is unspecified; the command validates before it asks.
    [[nodiscard]] Position after(Move move) const noexcept;

    /// The repetition key: the board, the side to move, the castling rights and the en passant
    /// file — and not the clocks, which is the definition of "the same position" for the
    /// threefold rule and the thing most often got wrong about it.
    [[nodiscard]] std::uint64_t key() const noexcept;

    /// Neither side can deliver mate by any sequence of legal moves: king against king, king
    /// and one minor piece against king, or king and bishop against king and bishop on the
    /// same colour of square. Two knights are not included, because a mate exists even if it
    /// cannot be forced.
    [[nodiscard]] bool insufficient_material() const noexcept;
};

static_assert(sizeof(Position) == 72, "a position is seventy-two bytes, and a change here "
                                      "is a change to every copy-make in the library");

/// Legal move count to `depth`, the standard test of a move generator: every published number
/// is a fact about chess, not about this library.
[[nodiscard]] std::uint64_t perft(const Position& position, int depth);

}  // namespace atlas::chess
