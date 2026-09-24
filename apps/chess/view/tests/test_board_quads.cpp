// SPDX-License-Identifier: GPL-3.0-or-later
// What the board looks like, without a device: which quads, in which order, from which cells.
#include <atlas/chess/board_quads.hpp>
#include <atlas/chess/fen.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using atlas::chess::BoardLayout;
using atlas::chess::BoardMarks;
using atlas::chess::build_board_quads;
using atlas::chess::Move;
using atlas::chess::MoveList;
using atlas::chess::Piece;
using atlas::chess::piece_cell;
using atlas::chess::Position;
using atlas::chess::sheet_cell;
using atlas::chess::square;

TEST_CASE("the starting position is sixty-four squares and thirty-two pieces", "[chess][quads]") {
    std::vector<atlas::renderer::Quad> quads;
    build_board_quads(Position::start(), {}, BoardLayout{}, quads);
    CHECK(quads.size() == 96);

    // a1 is dark and h1 is light, which is the fact anybody can check on a real board.
    CHECK(quads[0].colour.r < quads[7].colour.r);
    // The pieces come last, so they draw over the squares and the marks.
    CHECK(quads[64].uv.position.y == 0.0F);     // a1's white rook, top row of the sheet
    CHECK(quads.back().uv.position.y == 0.5F);  // h8's black rook, bottom row
}

TEST_CASE("a piece is drawn from the cell its kind and colour name", "[chess][quads]") {
    CHECK(piece_cell(Piece::WhitePawn).position.x == 0.0F);
    CHECK(piece_cell(Piece::WhiteKing).position.x == sheet_cell(5, 0).position.x);
    CHECK(piece_cell(Piece::BlackKnight).position.x == sheet_cell(1, 1).position.x);
    CHECK(piece_cell(Piece::BlackKnight).position.y == 0.5F);
    CHECK(sheet_cell(7, 1).position.x + sheet_cell(7, 1).size.x == 1.0F);
}

TEST_CASE("marks draw between the squares and the pieces, one per target square",
          "[chess][quads]") {
    // A pawn about to promote has four moves to one square and one mark.
    const Position about_to = atlas::chess::parse_fen("1n5k/P7/8/8/8/8/8/K7 w - - 0 1").value();
    MoveList legal;
    about_to.generate_legal(legal);
    std::vector<Move> from_pawn;
    for (const Move move : legal) {
        if (move.from == square(0, 6)) {
            from_pawn.push_back(move);
        }
    }
    REQUIRE(from_pawn.size() == 8);  // a8 four ways, b8 four ways

    std::vector<atlas::renderer::Quad> quads;
    build_board_quads(about_to,
                      BoardMarks{.selected = square(0, 6),
                                 .targets = from_pawn,
                                 .last_move = Move{.from = square(1, 6), .to = square(1, 7)}},
                      BoardLayout{}, quads);
    // 64 squares, 2 last-move, 1 selected, 2 targets (a8 ring, b8 capture), 4 pieces.
    CHECK(quads.size() == 64 + 2 + 1 + 2 + 4);
    // The a8 ring: an empty target is drawn from the ring cell.
    CHECK(quads[67].uv.position.x == sheet_cell(atlas::chess::kRingCell, 0).position.x);
    // The b8 capture: an occupied target is a tint of the solid cell.
    CHECK(quads[68].uv.position.x == sheet_cell(atlas::chess::kSolidCell, 0).position.x);
}

TEST_CASE("building appends and does not clear", "[chess][quads]") {
    std::vector<atlas::renderer::Quad> quads;
    build_board_quads(Position::start(), {}, BoardLayout{}, quads);
    build_board_quads(Position::start(), {}, BoardLayout{}, quads);
    CHECK(quads.size() == 192);
}
