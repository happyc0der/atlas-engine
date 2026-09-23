// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The vocabulary of a chess position: pieces, colours, squares. Plain integers with names,
/// because every one of them is authoritative state that is hashed and saved, and ADR-0008
/// wants explicit widths for that.
///
/// A square is an index from 0 (a1) to 63 (h8), rank-major: `rank * 8 + file`. A piece is one
/// byte whose low three bits are the kind and whose fourth bit is the colour, so a black piece
/// is its white counterpart plus eight and an empty square is zero. The gaps at 7, 8 and 15
/// are deliberate: a byte from a file that lands in one is not a piece, and `is_piece_code`
/// says so.

#include <cstdint>

namespace atlas::chess {

using Square = std::uint8_t;

inline constexpr Square kSquareCount = 64;
inline constexpr std::uint8_t kFileCount = 8;
inline constexpr std::uint8_t kRankCount = 8;

[[nodiscard]] constexpr Square square(std::uint8_t file, std::uint8_t rank) noexcept {
    return static_cast<Square>((rank * kFileCount) + file);
}

[[nodiscard]] constexpr std::uint8_t file_of(Square sq) noexcept {
    return static_cast<std::uint8_t>(sq % kFileCount);
}

[[nodiscard]] constexpr std::uint8_t rank_of(Square sq) noexcept {
    return static_cast<std::uint8_t>(sq / kFileCount);
}

enum class Colour : std::uint8_t { White = 0, Black = 1 };

[[nodiscard]] constexpr Colour other(Colour colour) noexcept {
    return colour == Colour::White ? Colour::Black : Colour::White;
}

enum class PieceKind : std::uint8_t {
    None = 0,
    Pawn = 1,
    Knight = 2,
    Bishop = 3,
    Rook = 4,
    Queen = 5,
    King = 6,
};

/// The bit that makes a piece black.
inline constexpr std::uint8_t kBlackBit = 8;

enum class Piece : std::uint8_t {
    None = 0,
    WhitePawn = 1,
    WhiteKnight = 2,
    WhiteBishop = 3,
    WhiteRook = 4,
    WhiteQueen = 5,
    WhiteKing = 6,
    BlackPawn = 9,
    BlackKnight = 10,
    BlackBishop = 11,
    BlackRook = 12,
    BlackQueen = 13,
    BlackKing = 14,
};

[[nodiscard]] constexpr Piece make_piece(Colour colour, PieceKind kind) noexcept {
    if (kind == PieceKind::None) {
        return Piece::None;
    }
    const unsigned bits =
        static_cast<unsigned>(kind) | (colour == Colour::Black ? unsigned{kBlackBit} : 0U);
    return static_cast<Piece>(static_cast<std::uint8_t>(bits));
}

[[nodiscard]] constexpr PieceKind kind_of(Piece piece) noexcept {
    return static_cast<PieceKind>(static_cast<std::uint8_t>(piece) & 7U);
}

[[nodiscard]] constexpr Colour colour_of(Piece piece) noexcept {
    return (static_cast<std::uint8_t>(piece) & kBlackBit) != 0 ? Colour::Black : Colour::White;
}

/// Whether a byte read from a file is a piece this library knows. Zero is a valid empty square.
[[nodiscard]] constexpr bool is_piece_code(std::uint8_t code) noexcept {
    const std::uint8_t kind = code & 7U;
    const std::uint8_t rest = code & static_cast<std::uint8_t>(~(7U | kBlackBit));
    if (rest != 0) {
        return false;
    }
    if (code == 0) {
        return true;
    }
    return kind >= 1 && kind <= 6;
}

/// Castling rights, as bits in one byte.
inline constexpr std::uint8_t kWhiteKingSide = 1;
inline constexpr std::uint8_t kWhiteQueenSide = 2;
inline constexpr std::uint8_t kBlackKingSide = 4;
inline constexpr std::uint8_t kBlackQueenSide = 8;
/// All four, written as the number rather than the OR: a bitwise operator on two `uint8_t`s
/// promotes them to `int`, and the newer clang-tidy on the Linux runner objects to that.
inline constexpr std::uint8_t kAllCastling = 0x0F;
static_assert(kAllCastling ==
              (kWhiteKingSide + kWhiteQueenSide + kBlackKingSide + kBlackQueenSide));

/// The en passant file when there is none. Files are 0 to 7.
inline constexpr std::uint8_t kNoEnPassant = 8;

}  // namespace atlas::chess
