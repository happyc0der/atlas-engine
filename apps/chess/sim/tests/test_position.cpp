// SPDX-License-Identifier: GPL-3.0-or-later
// The rules of movement, one at a time, on positions named by FEN.
#include <atlas/chess/fen.hpp>
#include <atlas/chess/position.hpp>

#include <catch2/catch_test_macros.hpp>

using atlas::chess::Colour;
using atlas::chess::Move;
using atlas::chess::MoveList;
using atlas::chess::parse_fen;
using atlas::chess::parse_move;
using atlas::chess::Piece;
using atlas::chess::PieceKind;
using atlas::chess::Position;
using atlas::chess::square;
using atlas::chess::to_fen;

namespace {

[[nodiscard]] Position pos(std::string_view fen) {
    auto parsed = parse_fen(fen);
    REQUIRE(parsed.has_value());
    return *parsed;
}

[[nodiscard]] Move mv(std::string_view text) {
    auto parsed = parse_move(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

[[nodiscard]] std::size_t legal_count(const Position& position) {
    MoveList moves;
    position.generate_legal(moves);
    return moves.size();
}

}  // namespace

TEST_CASE("the starting position has twenty legal moves and its FEN", "[chess][position]") {
    const Position start = Position::start();
    CHECK(legal_count(start) == 20);
    CHECK(to_fen(start) == "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    CHECK(pos("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") == start);
    CHECK_FALSE(start.in_check(Colour::White));
    CHECK(start.king_square(Colour::Black) == square(4, 7));
}

TEST_CASE("a move updates the board, the side, and the clocks", "[chess][position]") {
    const Position after_e4 = Position::start().after(mv("e2e4"));
    CHECK(after_e4.at(square(4, 3)) == Piece::WhitePawn);
    CHECK(after_e4.at(square(4, 1)) == Piece::None);
    CHECK(after_e4.side_to_move == Colour::Black);
    CHECK(after_e4.halfmove_clock == 0);
    CHECK(after_e4.fullmove_number == 1);
    // No black pawn can capture on e3, so no en passant file is recorded.
    CHECK(after_e4.en_passant_file == atlas::chess::kNoEnPassant);

    const Position after_nf3 = after_e4.after(mv("e7e5")).after(mv("g1f3"));
    CHECK(after_nf3.halfmove_clock == 1);
    CHECK(after_nf3.fullmove_number == 2);
    CHECK(after_nf3.side_to_move == Colour::Black);
}

TEST_CASE("en passant is offered only when a pawn can take, and taking removes the pawn",
          "[chess][position]") {
    // White pawn on e5, black plays d7d5: the file is recorded, and exd6 takes the d5 pawn.
    const Position before = pos("rnbqkbnr/ppp1pppp/8/3pP3/8/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 3");
    CHECK(before.en_passant_file == 3);
    const Move take = mv("e5d6");
    CHECK(before.is_legal(take));
    const Position after = before.after(take);
    CHECK(after.at(square(3, 5)) == Piece::WhitePawn);
    CHECK(after.at(square(3, 4)) == Piece::None);
    CHECK(after.at(square(4, 4)) == Piece::None);
    CHECK(after.halfmove_clock == 0);

    // The same FEN with no white pawn beside d5 parses with no en passant file at all.
    const Position nobody = pos("rnbqkbnr/ppp1pppp/8/3p4/8/8/PPPPPPPP/RNBQKBNR w KQkq d6 0 3");
    CHECK(nobody.en_passant_file == atlas::chess::kNoEnPassant);

    // And the offer lasts one move: after any other move it is gone.
    CHECK(before.after(mv("a2a3")).en_passant_file == atlas::chess::kNoEnPassant);
}

TEST_CASE("castling moves the rook, spends the rights, and is refused through check",
          "[chess][position]") {
    const Position ready = pos("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    CHECK(ready.is_legal(mv("e1g1")));
    CHECK(ready.is_legal(mv("e1c1")));

    const Position short_castled = ready.after(mv("e1g1"));
    CHECK(short_castled.at(square(6, 0)) == Piece::WhiteKing);
    CHECK(short_castled.at(square(5, 0)) == Piece::WhiteRook);
    CHECK(short_castled.at(square(7, 0)) == Piece::None);
    CHECK((short_castled.castling &
           (atlas::chess::kWhiteKingSide | atlas::chess::kWhiteQueenSide)) == 0);
    CHECK((short_castled.castling & atlas::chess::kBlackKingSide) != 0);

    const Position long_castled = ready.after(mv("e1c1"));
    CHECK(long_castled.at(square(2, 0)) == Piece::WhiteKing);
    CHECK(long_castled.at(square(3, 0)) == Piece::WhiteRook);
    CHECK(long_castled.at(square(0, 0)) == Piece::None);

    // A rook attacking f1 forbids short castling and not long; one attacking b1 forbids
    // neither, because the king does not cross b1.
    CHECK_FALSE(pos("r3k2r/8/8/8/8/8/5r2/R3K2R w KQkq - 0 1").is_legal(mv("e1g1")));
    CHECK(pos("r3k2r/8/8/8/8/8/5r2/R3K2R w KQkq - 0 1").is_legal(mv("e1c1")));
    CHECK(pos("r3k2r/8/8/8/8/8/1r6/R3K2R w KQkq - 0 1").is_legal(mv("e1c1")));
    // In check: no castling at all.
    CHECK_FALSE(pos("r3k2r/8/8/8/8/8/4r3/R3K2R w KQkq - 0 1").is_legal(mv("e1g1")));
    CHECK_FALSE(pos("r3k2r/8/8/8/8/8/4r3/R3K2R w KQkq - 0 1").is_legal(mv("e1c1")));

    // Moving a rook spends its right; the other survives.
    const Position rook_moved = ready.after(mv("h1h2"));
    CHECK((rook_moved.castling & atlas::chess::kWhiteKingSide) == 0);
    CHECK((rook_moved.castling & atlas::chess::kWhiteQueenSide) != 0);
    // Capturing a rook on its corner spends the opponent's right.
    const Position rook_taken = pos("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1").after(mv("h1h8"));
    CHECK((rook_taken.castling & atlas::chess::kBlackKingSide) == 0);
    CHECK((rook_taken.castling & atlas::chess::kBlackQueenSide) != 0);
}

TEST_CASE("promotion offers four pieces and a capture promotes too", "[chess][position]") {
    const Position about_to = pos("1n6/P7/8/8/8/8/8/k6K w - - 0 1");
    MoveList moves;
    about_to.generate_legal(moves);
    int promotions = 0;
    for (const Move move : moves) {
        if (move.promotion != PieceKind::None) {
            ++promotions;
        }
    }
    // a7a8 four ways, a7xb8 four ways.
    CHECK(promotions == 8);
    CHECK(about_to.after(mv("a7a8q")).at(square(0, 7)) == Piece::WhiteQueen);
    CHECK(about_to.after(mv("a7b8n")).at(square(1, 7)) == Piece::WhiteKnight);
    // A pawn cannot reach the last rank without promoting.
    CHECK_FALSE(about_to.is_legal(mv("a7a8")));
}

TEST_CASE("a move that leaves the king in check is not legal", "[chess][position]") {
    // The e-file bishop is pinned against the king by a rook.
    const Position pinned = pos("4r2k/8/8/8/8/8/4B3/4K3 w - - 0 1");
    CHECK_FALSE(pinned.is_legal(mv("e2d3")));
    CHECK_FALSE(pinned.is_legal(mv("e2f3")));
    // A bishop pinned along a file has nowhere to go at all; only the king moves.
    MoveList pinned_moves;
    pinned.generate_legal(pinned_moves);
    for (const Move move : pinned_moves) {
        CHECK(move.from == square(4, 0));
    }
    CHECK(pinned.is_legal(mv("e1d1")));
    // In check, only moves that answer it.
    const Position checked = pos("4r2k/8/8/8/8/8/8/4K3 w - - 0 1");
    CHECK(checked.in_check(Colour::White));
    MoveList moves;
    checked.generate_legal(moves);
    for (const Move move : moves) {
        CHECK_FALSE(checked.after(move).in_check(Colour::White));
    }
    CHECK(moves.size() == 4);  // d1, f1, d2, f2
}

TEST_CASE("checkmate and stalemate are no legal moves, in check and not", "[chess][position]") {
    // Fool's mate.
    const Position fools =
        Position::start().after(mv("f2f3")).after(mv("e7e5")).after(mv("g2g4")).after(mv("d8h4"));
    CHECK(fools.in_check(Colour::White));
    CHECK(legal_count(fools) == 0);
    // A classic stalemate: black king on a8, white queen on b6, white king on c7... black to
    // move has nothing.
    const Position stale = pos("k7/2K5/1Q6/8/8/8/8/8 b - - 0 1");
    CHECK_FALSE(stale.in_check(Colour::Black));
    CHECK(legal_count(stale) == 0);
}

TEST_CASE("the repetition key ignores the clocks and sees everything else", "[chess][position]") {
    const Position a = pos("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    Position b = a;
    b.halfmove_clock = 40;
    b.fullmove_number = 90;
    CHECK(a.key() == b.key());
    Position rights = a;
    rights.castling = 0;
    CHECK(rights.key() != a.key());
    Position side = a;
    side.side_to_move = Colour::Black;
    CHECK(side.key() != a.key());
    Position ep = a;
    ep.en_passant_file = 2;
    CHECK(ep.key() != a.key());
    Position moved = a.after(mv("a1a2")).after(mv("a8a7")).after(mv("a2a1")).after(mv("a7a8"));
    // Same squares, but the rooks moved, so the rights are gone: not the same position.
    CHECK(moved.squares == a.squares);
    CHECK(moved.key() != a.key());
}

TEST_CASE("insufficient material is recognised and not over-recognised", "[chess][position]") {
    using atlas::chess::Position;
    CHECK(pos("k7/8/8/8/8/8/8/K7 w - - 0 1").insufficient_material());
    CHECK(pos("k7/8/8/8/8/8/8/KN6 w - - 0 1").insufficient_material());
    CHECK(pos("k7/8/8/8/8/8/8/KB6 w - - 0 1").insufficient_material());
    // Bishops on the same colour: c1 and f8 are both dark... c1 is dark, f8 is dark.
    CHECK(pos("k4b2/8/8/8/8/8/8/K1B5 w - - 0 1").insufficient_material());
    // Opposite colours: c1 dark, e8 light.
    CHECK_FALSE(pos("k3b3/8/8/8/8/8/8/K1B5 w - - 0 1").insufficient_material());
    CHECK_FALSE(pos("k7/8/8/8/8/8/8/KNN5 w - - 0 1").insufficient_material());
    CHECK_FALSE(pos("k7/8/8/8/8/8/P7/K7 w - - 0 1").insufficient_material());
    CHECK_FALSE(pos("k7/8/8/8/8/8/8/KR6 w - - 0 1").insufficient_material());
}

TEST_CASE("tables and positions round-trip", "[chess][position]") {
    atlas::chess::BoardTable board;
    atlas::chess::StateTable state;
    const Position original = pos("r3k2r/pp3ppp/8/8/8/8/PP3PPP/R3K2R b Kq - 7 21");
    original.write_tables(board, state);
    CHECK(Position::from_tables(board, state) == original);
    CHECK(state.halfmove_clock == 7);
    CHECK(state.fullmove_number == 21);
}
