// SPDX-License-Identifier: GPL-3.0-or-later
// Where squares are, and which square a click lands on — in both orientations, at the edges,
// and off the board.
#include <atlas/chess/board_layout.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using atlas::chess::BoardLayout;
using atlas::chess::square;
using atlas::math::Vec2;

TEST_CASE("white at the bottom puts a1 bottom-left and h8 top-right", "[chess][layout]") {
    const BoardLayout layout;
    // y grows downwards, so the bottom row is y = 7.
    CHECK(layout.square_rect(square(0, 0)).position.x == 0.0F);
    CHECK(layout.square_rect(square(0, 0)).position.y == 7.0F);
    CHECK(layout.square_rect(square(7, 7)).position.x == 7.0F);
    CHECK(layout.square_rect(square(7, 7)).position.y == 0.0F);
    CHECK(layout.square_rect(square(4, 3)).size.x == 1.0F);
}

TEST_CASE("flipped puts black at the bottom", "[chess][layout]") {
    const BoardLayout layout{.flipped = true};
    CHECK(layout.square_rect(square(0, 0)).position.x == 7.0F);
    CHECK(layout.square_rect(square(0, 0)).position.y == 0.0F);
    CHECK(layout.square_rect(square(7, 7)).position.x == 0.0F);
    CHECK(layout.square_rect(square(7, 7)).position.y == 7.0F);
}

TEST_CASE("every square's own centre picks that square, in both orientations", "[chess][layout]") {
    for (const bool flipped : {false, true}) {
        const BoardLayout layout{.flipped = flipped};
        for (atlas::chess::Square sq = 0; sq < atlas::chess::kSquareCount; ++sq) {
            const auto rect = layout.square_rect(sq);
            const Vec2 centre{.x = rect.position.x + 0.5F, .y = rect.position.y + 0.5F};
            INFO("square " << static_cast<int>(sq) << " flipped " << flipped);
            CHECK(layout.square_at(centre) == sq);
        }
    }
}

TEST_CASE("an edge belongs to exactly one square and off the board is nothing", "[chess][layout]") {
    const BoardLayout layout;
    // x = 1 is the line between the a- and b-files: half-open, so it is the b-file.
    CHECK(layout.square_at({.x = 1.0F, .y = 7.5F}) == square(1, 0));
    CHECK(layout.square_at({.x = 0.0F, .y = 0.0F}) == square(0, 7));
    CHECK_FALSE(layout.square_at({.x = 8.0F, .y = 3.0F}).has_value());
    CHECK_FALSE(layout.square_at({.x = -0.001F, .y = 3.0F}).has_value());
    CHECK_FALSE(layout.square_at({.x = 3.0F, .y = 8.0F}).has_value());
    CHECK_FALSE(
        layout.square_at({.x = std::numeric_limits<float>::quiet_NaN(), .y = 1.0F}).has_value());
}
