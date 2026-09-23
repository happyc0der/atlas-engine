// SPDX-License-Identifier: GPL-3.0-or-later
// Notation as untrusted input: what parses, what is refused, and that both directions agree.
#include <atlas/chess/fen.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using atlas::chess::parse_fen;
using atlas::chess::parse_move;
using atlas::chess::to_fen;
using atlas::chess::to_string;

TEST_CASE("well-known FENs round-trip through parse and print", "[chess][fen]") {
    for (const std::string fen : {
             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
             "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
             "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
             "1n1Rkb1r/p4ppp/4q3/4p1B1/4P3/8/PPP2PPP/2K5 b - - 1 17",
             "rnbqkbnr/ppp1pppp/8/3pP3/8/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 3",
         }) {
        INFO(fen);
        auto parsed = parse_fen(fen);
        REQUIRE(parsed.has_value());
        CHECK(to_fen(*parsed) == fen);
    }
}

TEST_CASE("the clocks are optional and default", "[chess][fen]") {
    auto parsed = parse_fen("8/8/8/8/8/8/8/k6K w - -");
    REQUIRE(parsed.has_value());
    CHECK(parsed->halfmove_clock == 0);
    CHECK(parsed->fullmove_number == 1);
}

TEST_CASE("an en passant square nobody can use is not recorded", "[chess][fen]") {
    auto parsed = parse_fen("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");
    REQUIRE(parsed.has_value());
    CHECK(parsed->en_passant_file == atlas::chess::kNoEnPassant);
    CHECK(to_fen(*parsed) == "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");
}

TEST_CASE("malformed FENs are refused", "[chess][fen]") {
    for (const std::string fen : {
             "",
             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR",                 // too few fields
             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 x",  // too many
             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBN w KQkq - 0 1",     // short rank
             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNRR w KQkq - 0 1",   // long rank
             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP w KQkq - 0 1",             // seven ranks
             "rnbqkbnr/pppppppp/8/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",  // nine ranks
             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNX w KQkq - 0 1",    // not a piece
             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x KQkq - 0 1",    // side
             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkz - 0 1",    // castling letter
             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KK - 0 1",      // right twice
             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq z9 0 1",   // square
             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq e4 0 1",   // wrong rank
             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 101 1",  // past fifty moves
             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 0",    // fullmove zero
             "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - x 1",    // not a number
         }) {
        INFO(fen);
        CHECK_FALSE(parse_fen(fen).has_value());
    }
    CHECK_FALSE(parse_fen(std::string(129, 'k')).has_value());
}

TEST_CASE("moves round-trip and refuse what is not a move", "[chess][fen]") {
    for (const std::string text : {"e2e4", "e7e8q", "a1h8", "b7b8n"}) {
        auto parsed = parse_move(text);
        REQUIRE(parsed.has_value());
        CHECK(to_string(*parsed) == text);
    }
    for (const std::string text : {"", "e2", "e2e", "e2e4x", "i2i4", "e9e4", "e7e8k"}) {
        INFO(text);
        CHECK_FALSE(parse_move(text).has_value());
    }
}
