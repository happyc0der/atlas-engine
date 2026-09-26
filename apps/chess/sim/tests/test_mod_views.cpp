// SPDX-License-Identifier: GPL-3.0-or-later
// The position view's bytes, at the offsets `mod_view.h` names (ADR-0023 D7).
//
// The mod reads these offsets and the application writes them, from one header in two languages.
// What this checks is that the writer puts each field where the header says, little-endian where
// it says, including values past one byte, so a mod reading a clock past 255 reads it whole.
#include <atlas/chess/fen.hpp>
#include <atlas/chess/mod_views.hpp>
#include <atlas/chess/rules.hpp>
#include <atlas/chess/world.hpp>

#include "chess_harness.hpp"
#include <catch2/catch_test_macros.hpp>

using atlas::chess::testing::ChessHarness;

TEST_CASE("the position view puts every field where mod_view.h says", "[chess][views]") {
    ChessHarness chess;
    // Black to move, one castling right, an en passant file, and clocks past a byte.
    const auto position = atlas::chess::parse_fen("4k2r/8/8/8/3pP3/8/8/4K3 b k e3 70 300");
    REQUIRE(position.has_value());
    atlas::chess::set_position(chess.world(), chess.ids(), *position);

    const auto view = atlas::chess::position_view(chess.world(), chess.ids());
    for (std::size_t sq = 0; sq < 64; ++sq) {
        CHECK(view[ATLAS_CHESS_VIEW_SQUARES + sq] == position->squares[sq]);
    }
    CHECK(view[ATLAS_CHESS_VIEW_SIDE] == 1);
    CHECK(view[ATLAS_CHESS_VIEW_CASTLING] == atlas::chess::kBlackKingSide);
    CHECK(view[ATLAS_CHESS_VIEW_EN_PASSANT] == 4);
    CHECK(view[ATLAS_CHESS_VIEW_HALFMOVE] == 70);
    CHECK(view[ATLAS_CHESS_VIEW_HALFMOVE + 1] == 0);
    CHECK(view[ATLAS_CHESS_VIEW_FULLMOVE] == (300 & 0xFF));
    CHECK(view[ATLAS_CHESS_VIEW_FULLMOVE + 1] == (300 >> 8));
    CHECK(view[ATLAS_CHESS_VIEW_OUTCOME] == 0);
}

TEST_CASE("the position view carries the outcome once the game is over", "[chess][views]") {
    ChessHarness chess;
    const auto position = atlas::chess::parse_fen("6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1");
    REQUIRE(position.has_value());
    atlas::chess::set_position(chess.world(), chess.ids(), *position);
    auto kernel = chess.kernel(1);
    REQUIRE(chess.submit(0, atlas::chess::parse_move("a1a8").value()).has_value());
    REQUIRE(kernel.step().has_value());

    const auto view = atlas::chess::position_view(chess.world(), chess.ids());
    CHECK(view[ATLAS_CHESS_VIEW_OUTCOME] ==
          static_cast<std::uint8_t>(atlas::chess::Outcome::WhiteWins));
}
