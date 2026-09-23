// SPDX-License-Identifier: GPL-3.0-or-later
// The five tables composed into a world, ticked by a kernel with no systems, saved and loaded
// through the engine's own paths — the first thing the probe has to prove, before a rule
// exists: that a game's state can live in the engine's world at all.
#include <atlas/chess/world.hpp>
#include <atlas/simulation/save.hpp>

#include "chess_harness.hpp"
#include <catch2/catch_test_macros.hpp>

using atlas::chess::board_table;
using atlas::chess::Colour;
using atlas::chess::history_table;
using atlas::chess::Piece;
using atlas::chess::players_table;
using atlas::chess::square;
using atlas::chess::state_table;
using atlas::chess::validate_world;
using atlas::chess::testing::ChessHarness;

TEST_CASE("a fresh world holds the starting position in five tables", "[chess][world]") {
    ChessHarness h;
    CHECK(h.world().table_count() == 5);
    const auto& board = board_table(h.world(), h.ids());
    CHECK(board.at(square(4, 0)) == Piece::WhiteKing);
    CHECK(board.at(square(3, 7)) == Piece::BlackQueen);
    CHECK(board.at(square(0, 1)) == Piece::WhitePawn);
    CHECK(board.at(square(7, 6)) == Piece::BlackPawn);
    CHECK(board.at(square(4, 4)) == Piece::None);
    int pieces = 0;
    for (const auto code : board.squares) {
        pieces += code != 0 ? 1 : 0;
    }
    CHECK(pieces == 32);
    CHECK(state_table(h.world(), h.ids()).side_to_move == Colour::White);
    CHECK(state_table(h.world(), h.ids()).castling == atlas::chess::kAllCastling);
    // The history starts with the starting position's own key, so a repetition of it counts.
    CHECK(history_table(h.world(), h.ids()).keys.size() == 1);
    CHECK(players_table(h.world(), h.ids()).white == atlas::sim::SourceId::Local);
    CHECK(validate_world(h.world(), h.ids()).has_value());
}

TEST_CASE("two fresh worlds hash alike and a moved piece changes the hash", "[chess][world]") {
    ChessHarness a;
    ChessHarness b;
    CHECK(a.world().hash() == b.world().hash());
    auto& board = board_table(a.world(), a.ids());
    board.put(square(4, 3), Piece::WhitePawn);
    board.put(square(4, 1), Piece::None);
    CHECK(a.world().hash() != b.world().hash());
}

TEST_CASE("an empty schedule ticks and the position is untouched by ticking",
          "[chess][world][determinism]") {
    // No systems, so a tick changes nothing but the tick: the hash after ten steps is the hash
    // before them. A position changes only through a command, which is what the tick rule
    // says and what an empty schedule makes literal.
    ChessHarness h;
    auto kernel = h.kernel(1);
    const auto before = h.world().hash();
    const auto reports = kernel.run(10);
    REQUIRE(reports.has_value());
    REQUIRE(reports->size() == 10);
    CHECK(kernel.current_tick() == 10);
    CHECK(h.world().hash() == before);
    for (const auto& report : *reports) {
        CHECK(report.state_hash == before);
        CHECK(report.commands_applied == 0);
    }
}

TEST_CASE("a saved chess world loads back with the same hash", "[chess][world][save]") {
    ChessHarness source;
    auto kernel = source.kernel(5);
    REQUIRE(kernel.run(3).has_value());
    auto& board = board_table(source.world(), source.ids());
    board.put(square(4, 3), Piece::WhitePawn);
    board.put(square(4, 1), Piece::None);
    state_table(source.world(), source.ids()).side_to_move = Colour::Black;
    state_table(source.world(), source.ids()).en_passant_file = 4;
    players_table(source.world(), source.ids()).black = atlas::sim::SourceId{1};
    const auto bytes = atlas::sim::save(source.world(), kernel, source.commands).value();

    ChessHarness target;
    auto target_kernel = target.kernel(0);
    REQUIRE(atlas::sim::load(target.world(), target_kernel, target.commands, bytes).has_value());
    CHECK(target.world().hash() == source.world().hash());
    CHECK(target_kernel.current_tick() == 3);
    CHECK(board_table(target.world(), target.ids()).at(square(4, 3)) == Piece::WhitePawn);
    CHECK(players_table(target.world(), target.ids()).black == atlas::sim::SourceId{1});
    CHECK(validate_world(target.world(), target.ids()).has_value());
}

TEST_CASE("a truncated save is refused and the world is unchanged", "[chess][world][save]") {
    ChessHarness source;
    auto kernel = source.kernel(5);
    REQUIRE(kernel.run(2).has_value());
    auto bytes = atlas::sim::save(source.world(), kernel, source.commands).value();
    bytes.resize(bytes.size() - 7);

    ChessHarness target;
    auto target_kernel = target.kernel(0);
    const auto before = target.world().hash();
    CHECK_FALSE(
        atlas::sim::load(target.world(), target_kernel, target.commands, bytes).has_value());
    CHECK(target.world().hash() == before);
    CHECK(target_kernel.current_tick() == 0);
}

TEST_CASE("validation sees what no single table can", "[chess][world]") {
    ChessHarness h;
    auto& board = board_table(h.world(), h.ids());
    auto& state = state_table(h.world(), h.ids());
    auto& history = history_table(h.world(), h.ids());

    SECTION("two white kings") {
        board.put(square(4, 4), Piece::WhiteKing);
        CHECK_FALSE(validate_world(h.world(), h.ids()).has_value());
    }
    SECTION("no black king") {
        board.put(square(4, 7), Piece::None);
        CHECK_FALSE(validate_world(h.world(), h.ids()).has_value());
    }
    SECTION("a pawn on the eighth rank") {
        board.put(square(2, 7), Piece::WhitePawn);
        CHECK_FALSE(validate_world(h.world(), h.ids()).has_value());
    }
    SECTION("a castling right after the rook has gone") {
        board.put(square(7, 0), Piece::None);
        CHECK_FALSE(validate_world(h.world(), h.ids()).has_value());
        state.castling = static_cast<std::uint8_t>(state.castling & ~atlas::chess::kWhiteKingSide);
        CHECK(validate_world(h.world(), h.ids()).has_value());
    }
    SECTION("an en passant file with no pawn behind it") {
        state.en_passant_file = 4;
        CHECK_FALSE(validate_world(h.world(), h.ids()).has_value());
        // Black to move after white's e-pawn went two squares: the pawn is on e4, e2 and e3
        // are empty.
        state.side_to_move = Colour::Black;
        board.put(square(4, 1), Piece::None);
        board.put(square(4, 3), Piece::WhitePawn);
        CHECK(validate_world(h.world(), h.ids()).has_value());
    }
    SECTION("a history longer than the clock allows, or empty") {
        history.keys = {1, 2, 3};
        state.halfmove_clock = 1;
        CHECK_FALSE(validate_world(h.world(), h.ids()).has_value());
        state.halfmove_clock = 2;
        CHECK(validate_world(h.world(), h.ids()).has_value());
        // Fewer keys than plies is a position set up mid-game, which is allowed.
        state.halfmove_clock = 40;
        CHECK(validate_world(h.world(), h.ids()).has_value());
        history.keys.clear();
        CHECK_FALSE(validate_world(h.world(), h.ids()).has_value());
    }
}
