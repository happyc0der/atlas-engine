// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/chess/position.hpp>
#include <atlas/core/assert.hpp>
#include <atlas/core/hash.hpp>

#include <cstdlib>
#include <span>

namespace atlas::chess {
namespace {

struct Delta {
    int file;
    int rank;
};

constexpr std::array<Delta, 8> kKnightDeltas{
    Delta{1, 2},   Delta{2, 1},   Delta{2, -1}, Delta{1, -2},
    Delta{-1, -2}, Delta{-2, -1}, Delta{-2, 1}, Delta{-1, 2},
};
constexpr std::array<Delta, 8> kKingDeltas{
    Delta{1, 0},  Delta{1, 1},   Delta{0, 1},  Delta{-1, 1},
    Delta{-1, 0}, Delta{-1, -1}, Delta{0, -1}, Delta{1, -1},
};
constexpr std::array<Delta, 4> kBishopDeltas{Delta{1, 1}, Delta{-1, 1}, Delta{-1, -1},
                                             Delta{1, -1}};
constexpr std::array<Delta, 4> kRookDeltas{Delta{1, 0}, Delta{0, 1}, Delta{-1, 0}, Delta{0, -1}};

/// The back rank, king's side to the right: rook, knight, bishop, queen, king, bishop, knight,
/// rook. Files a to h.
constexpr std::array<PieceKind, kFileCount> kBackRank{
    PieceKind::Rook, PieceKind::Knight, PieceKind::Bishop, PieceKind::Queen,
    PieceKind::King, PieceKind::Bishop, PieceKind::Knight, PieceKind::Rook,
};

constexpr int kFiles = kFileCount;
constexpr int kRanks = kRankCount;

[[nodiscard]] constexpr bool on_board(int file, int rank) noexcept {
    return file >= 0 && file < kFiles && rank >= 0 && rank < kRanks;
}

[[nodiscard]] constexpr Square sq_at(int file, int rank) noexcept {
    return square(static_cast<std::uint8_t>(file), static_cast<std::uint8_t>(rank));
}

/// The rank a colour's pawns start on, the direction they move, and the rank they promote on.
[[nodiscard]] constexpr int pawn_start_rank(Colour colour) noexcept {
    return colour == Colour::White ? 1 : 6;
}

[[nodiscard]] constexpr int pawn_direction(Colour colour) noexcept {
    return colour == Colour::White ? 1 : -1;
}

[[nodiscard]] constexpr int promotion_rank(Colour colour) noexcept {
    return colour == Colour::White ? 7 : 0;
}

[[nodiscard]] constexpr int home_rank(Colour colour) noexcept {
    return colour == Colour::White ? 0 : 7;
}

/// The castling right lost when a rook leaves or is taken from a corner, or nothing.
[[nodiscard]] constexpr std::uint8_t right_of_corner(Square sq) noexcept {
    if (sq == square(7, 0)) {
        return kWhiteKingSide;
    }
    if (sq == square(0, 0)) {
        return kWhiteQueenSide;
    }
    if (sq == square(7, 7)) {
        return kBlackKingSide;
    }
    if (sq == square(0, 7)) {
        return kBlackQueenSide;
    }
    return 0;
}

constexpr std::array<PieceKind, 4> kPromotions{PieceKind::Queen, PieceKind::Rook, PieceKind::Bishop,
                                               PieceKind::Knight};

}  // namespace

bool MoveList::contains(Move move) const noexcept {
    for (std::size_t i = 0; i < m_count; ++i) {
        if (m_moves[i] == move) {
            return true;
        }
    }
    return false;
}

Position Position::from_tables(const BoardTable& board, const StateTable& state) {
    Position position;
    position.squares = board.squares;
    position.side_to_move = state.side_to_move;
    position.castling = state.castling;
    position.en_passant_file = state.en_passant_file;
    position.halfmove_clock = state.halfmove_clock;
    position.fullmove_number = state.fullmove_number;
    return position;
}

void Position::write_tables(BoardTable& board, StateTable& state) const {
    board.squares = squares;
    state.side_to_move = side_to_move;
    state.castling = castling;
    state.en_passant_file = en_passant_file;
    state.halfmove_clock = halfmove_clock;
    state.fullmove_number = fullmove_number;
}

Position Position::start() {
    Position position;
    for (std::uint8_t file = 0; file < kFileCount; ++file) {
        position.put(square(file, 0), make_piece(Colour::White, kBackRank[file]));
        position.put(square(file, 1), Piece::WhitePawn);
        position.put(square(file, 6), Piece::BlackPawn);
        position.put(square(file, 7), make_piece(Colour::Black, kBackRank[file]));
    }
    return position;
}

Square Position::king_square(Colour colour) const noexcept {
    const Piece king = make_piece(colour, PieceKind::King);
    for (Square sq = 0; sq < kSquareCount; ++sq) {
        if (at(sq) == king) {
            return sq;
        }
    }
    ATLAS_ASSERT_MSG(false, "a position without a king was asked where it is");
    return 0;
}

bool Position::is_attacked(Square sq, Colour by) const noexcept {
    const int file = file_of(sq);
    const int rank = rank_of(sq);

    // A pawn of `by` attacks diagonally forward, so it stands diagonally *behind* the square.
    const int pawn_rank = rank - pawn_direction(by);
    const Piece pawn = make_piece(by, PieceKind::Pawn);
    for (const int df : {-1, 1}) {
        if (on_board(file + df, pawn_rank) && at(sq_at(file + df, pawn_rank)) == pawn) {
            return true;
        }
    }

    const Piece knight = make_piece(by, PieceKind::Knight);
    for (const auto& d : kKnightDeltas) {
        if (on_board(file + d.file, rank + d.rank) &&
            at(sq_at(file + d.file, rank + d.rank)) == knight) {
            return true;
        }
    }

    const Piece king = make_piece(by, PieceKind::King);
    for (const auto& d : kKingDeltas) {
        if (on_board(file + d.file, rank + d.rank) &&
            at(sq_at(file + d.file, rank + d.rank)) == king) {
            return true;
        }
    }

    const auto ray_hits = [&](std::span<const Delta> deltas, PieceKind slider) {
        const Piece sliding = make_piece(by, slider);
        const Piece queen = make_piece(by, PieceKind::Queen);
        for (const auto& d : deltas) {
            int f = file + d.file;
            int r = rank + d.rank;
            while (on_board(f, r)) {
                const Piece piece = at(sq_at(f, r));
                if (piece != Piece::None) {
                    if (piece == sliding || piece == queen) {
                        return true;
                    }
                    break;
                }
                f += d.file;
                r += d.rank;
            }
        }
        return false;
    };
    return ray_hits(kBishopDeltas, PieceKind::Bishop) || ray_hits(kRookDeltas, PieceKind::Rook);
}

bool Position::in_check(Colour colour) const noexcept {
    return is_attacked(king_square(colour), other(colour));
}

void Position::generate_pseudo_legal(MoveList& out) const {
    const Colour us = side_to_move;
    const Colour them = other(us);
    const int dir = pawn_direction(us);

    const auto push_pawn_move = [&](Square from, Square to) {
        if (rank_of(to) == promotion_rank(us)) {
            for (const PieceKind kind : kPromotions) {
                out.push(Move{.from = from, .to = to, .promotion = kind});
            }
        } else {
            out.push(Move{.from = from, .to = to});
        }
    };
    const auto push_if_free_or_enemy = [&](Square from, int file, int rank) {
        if (!on_board(file, rank)) {
            return false;  // off the board: a ray stops
        }
        const Piece target = at(sq_at(file, rank));
        if (target == Piece::None) {
            out.push(Move{.from = from, .to = sq_at(file, rank)});
            return true;  // empty: a ray continues
        }
        if (colour_of(target) == them) {
            out.push(Move{.from = from, .to = sq_at(file, rank)});
        }
        return false;  // occupied either way: a ray stops
    };

    for (Square from = 0; from < kSquareCount; ++from) {
        const Piece piece = at(from);
        if (piece == Piece::None || colour_of(piece) != us) {
            continue;
        }
        const int file = file_of(from);
        const int rank = rank_of(from);

        switch (kind_of(piece)) {
        case PieceKind::Pawn: {
            const int ahead = rank + dir;
            if (on_board(file, ahead) && at(sq_at(file, ahead)) == Piece::None) {
                push_pawn_move(from, sq_at(file, ahead));
                const int two = rank + (2 * dir);
                if (rank == pawn_start_rank(us) && at(sq_at(file, two)) == Piece::None) {
                    out.push(Move{.from = from, .to = sq_at(file, two)});
                }
            }
            for (const int df : {-1, 1}) {
                if (!on_board(file + df, ahead)) {
                    continue;
                }
                const Square to = sq_at(file + df, ahead);
                const Piece target = at(to);
                if (target != Piece::None && colour_of(target) == them) {
                    push_pawn_move(from, to);
                } else if (target == Piece::None && en_passant_file == file + df &&
                           rank == (us == Colour::White ? 4 : 3)) {
                    out.push(Move{.from = from, .to = to});
                }
            }
            break;
        }
        case PieceKind::Knight:
            for (const auto& d : kKnightDeltas) {
                (void)push_if_free_or_enemy(from, file + d.file, rank + d.rank);
            }
            break;
        case PieceKind::Bishop:
        case PieceKind::Rook:
        case PieceKind::Queen: {
            const auto slide = [&](std::span<const Delta> deltas) {
                for (const auto& d : deltas) {
                    int f = file + d.file;
                    int r = rank + d.rank;
                    while (push_if_free_or_enemy(from, f, r)) {
                        f += d.file;
                        r += d.rank;
                    }
                }
            };
            if (kind_of(piece) != PieceKind::Rook) {
                slide(kBishopDeltas);
            }
            if (kind_of(piece) != PieceKind::Bishop) {
                slide(kRookDeltas);
            }
            break;
        }
        case PieceKind::King: {
            for (const auto& d : kKingDeltas) {
                (void)push_if_free_or_enemy(from, file + d.file, rank + d.rank);
            }
            // Castling: the right, the king on its square, the squares between empty, and
            // neither the square the king stands on nor the ones it crosses attacked. The
            // last is the rule itself, not a legality filter, so it belongs here.
            const int home = home_rank(us);
            if (from != sq_at(4, home) || is_attacked(from, them)) {
                break;
            }
            const std::uint8_t king_side = us == Colour::White ? kWhiteKingSide : kBlackKingSide;
            const std::uint8_t queen_side = us == Colour::White ? kWhiteQueenSide : kBlackQueenSide;
            const Piece rook = make_piece(us, PieceKind::Rook);
            if ((castling & king_side) != 0 && at(sq_at(7, home)) == rook &&
                at(sq_at(5, home)) == Piece::None && at(sq_at(6, home)) == Piece::None &&
                !is_attacked(sq_at(5, home), them) && !is_attacked(sq_at(6, home), them)) {
                out.push(Move{.from = from, .to = sq_at(6, home)});
            }
            if ((castling & queen_side) != 0 && at(sq_at(0, home)) == rook &&
                at(sq_at(1, home)) == Piece::None && at(sq_at(2, home)) == Piece::None &&
                at(sq_at(3, home)) == Piece::None && !is_attacked(sq_at(3, home), them) &&
                !is_attacked(sq_at(2, home), them)) {
                out.push(Move{.from = from, .to = sq_at(2, home)});
            }
            break;
        }
        case PieceKind::None: break;
        }
    }
}

void Position::generate_legal(MoveList& out) const {
    MoveList pseudo;
    generate_pseudo_legal(pseudo);
    for (const Move move : pseudo) {
        if (!after(move).in_check(side_to_move)) {
            out.push(move);
        }
    }
}

bool Position::is_legal(Move move) const {
    MoveList legal;
    generate_legal(legal);
    return legal.contains(move);
}

Position Position::after(Move move) const noexcept {
    Position next = *this;
    const Piece piece = at(move.from);
    const Colour us = colour_of(piece);
    const Colour them = other(us);
    const bool pawn = kind_of(piece) == PieceKind::Pawn;
    bool capture = at(move.to) != Piece::None;

    // En passant: a pawn moving diagonally onto an empty square takes the pawn beside it.
    if (pawn && file_of(move.from) != file_of(move.to) && !capture) {
        next.put(square(file_of(move.to), rank_of(move.from)), Piece::None);
        capture = true;
    }

    next.put(move.to, move.promotion != PieceKind::None ? make_piece(us, move.promotion) : piece);
    next.put(move.from, Piece::None);

    // Castling: the king moved two files, so the rook jumps over it.
    if (kind_of(piece) == PieceKind::King) {
        const int home = home_rank(us);
        if (move.to == sq_at(6, home) && move.from == sq_at(4, home)) {
            next.put(sq_at(7, home), Piece::None);
            next.put(sq_at(5, home), make_piece(us, PieceKind::Rook));
        } else if (move.to == sq_at(2, home) && move.from == sq_at(4, home)) {
            next.put(sq_at(0, home), Piece::None);
            next.put(sq_at(3, home), make_piece(us, PieceKind::Rook));
        }
        const std::uint8_t both = us == Colour::White ? (kWhiteKingSide | kWhiteQueenSide)
                                                      : (kBlackKingSide | kBlackQueenSide);
        next.castling = static_cast<std::uint8_t>(next.castling & (0xFFU ^ both));
    }
    // A rook leaving its corner, or a rook taken on its corner, ends that right.
    next.castling = static_cast<std::uint8_t>(next.castling & (0xFFU ^ right_of_corner(move.from)));
    next.castling = static_cast<std::uint8_t>(next.castling & (0xFFU ^ right_of_corner(move.to)));

    // The en passant file is recorded only when an enemy pawn stands ready to use it. FEN
    // conventionally records it after any double push; for the repetition key that would make
    // two identical positions differ by a capture nobody can make, so this library records
    // the file only when it means something.
    next.en_passant_file = kNoEnPassant;
    if (pawn &&
        std::abs(static_cast<int>(rank_of(move.to)) - static_cast<int>(rank_of(move.from))) == 2) {
        const Piece enemy_pawn = make_piece(them, PieceKind::Pawn);
        const int file = file_of(move.to);
        const int rank = rank_of(move.to);
        for (const int df : {-1, 1}) {
            if (on_board(file + df, rank) && at(sq_at(file + df, rank)) == enemy_pawn) {
                next.en_passant_file = static_cast<std::uint8_t>(file);
            }
        }
    }

    next.halfmove_clock = (pawn || capture) ? 0 : static_cast<std::uint16_t>(halfmove_clock + 1);
    if (us == Colour::Black) {
        ++next.fullmove_number;
    }
    next.side_to_move = them;
    return next;
}

std::uint64_t Position::key() const noexcept {
    Hasher hasher;
    hasher.add(std::as_bytes(std::span<const std::uint8_t>{squares}));
    hasher.add(side_to_move);
    hasher.add(castling);
    hasher.add(en_passant_file);
    return hasher.value();
}

bool Position::insufficient_material() const noexcept {
    int minors = 0;
    std::array<int, 2> bishops{0, 0};
    std::array<int, 2> bishop_square_colour{-1, -1};
    for (Square sq = 0; sq < kSquareCount; ++sq) {
        const Piece piece = at(sq);
        switch (kind_of(piece)) {
        case PieceKind::None:
        case PieceKind::King: break;
        case PieceKind::Pawn:
        case PieceKind::Rook:
        case PieceKind::Queen: return false;
        case PieceKind::Knight: ++minors; break;
        case PieceKind::Bishop: {
            ++minors;
            const auto side = static_cast<std::size_t>(colour_of(piece));
            ++bishops[side];
            bishop_square_colour[side] = (file_of(sq) + rank_of(sq)) % 2;
            break;
        }
        }
    }
    if (minors <= 1) {
        return true;
    }
    // Exactly one bishop each and nothing else: a draw when they never meet.
    return minors == 2 && bishops[0] == 1 && bishops[1] == 1 &&
           bishop_square_colour[0] == bishop_square_colour[1];
}

std::uint64_t perft(const Position& position, int depth) {
    if (depth <= 0) {
        return 1;
    }
    MoveList legal;
    position.generate_legal(legal);
    if (depth == 1) {
        return legal.size();
    }
    std::uint64_t nodes = 0;
    for (const Move move : legal) {
        nodes += perft(position.after(move), depth - 1);
    }
    return nodes;
}

}  // namespace atlas::chess
