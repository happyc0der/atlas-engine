// SPDX-License-Identifier: GPL-3.0-or-later
// Perft: the legal move count to a depth, against numbers that are facts about chess rather
// than about this library. A generator with any rule wrong disagrees with them somewhere,
// which is what makes this the test that finds every generation bug there is.
//
// Depths are chosen for a debug build under the address sanitiser; each position's deeper
// published counts are listed in the comment beside it for anyone who wants to run them.
#include <atlas/chess/fen.hpp>
#include <atlas/chess/position.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>

using atlas::chess::parse_fen;
using atlas::chess::perft;

namespace {

struct Case {
    std::string_view name;
    std::string_view fen;
    int depth;
    std::uint64_t nodes;
};

// Sources: the Chess Programming Wiki's perft results, which every published generator has
// reproduced.
constexpr std::array<Case, 6> kCases{{
    // 20, 400, 8902, 197281, 4865609
    {"start", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 4, 197'281},
    // 48, 2039, 97862, 4085603 — castling, promotions, en passant and checks all at once.
    {"kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3, 97'862},
    // 14, 191, 2812, 43238, 674624 — en passant discoveries and a pinned pawn.
    {"position 3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 4, 43'238},
    // 6, 264, 9467, 422333 — promotions with captures, and castling out of check refused.
    {"position 4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 3, 9'467},
    // 44, 1486, 62379, 2103487
    {"position 5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 3, 62'379},
    // 46, 2079, 89890, 3894594
    {"position 6", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 3,
     89'890},
}};

}  // namespace

TEST_CASE("perft matches the published counts", "[chess][perft]") {
    for (const auto& c : kCases) {
        INFO(c.name);
        auto position = parse_fen(c.fen);
        REQUIRE(position.has_value());
        CHECK(perft(*position, c.depth) == c.nodes);
    }
}

TEST_CASE("perft at every shallower depth matches too", "[chess][perft]") {
    // The deeper count could match by two errors cancelling; the shallow ones cannot.
    const auto start = parse_fen(kCases[0].fen).value();
    CHECK(perft(start, 1) == 20);
    CHECK(perft(start, 2) == 400);
    CHECK(perft(start, 3) == 8'902);
    const auto kiwipete = parse_fen(kCases[1].fen).value();
    CHECK(perft(kiwipete, 1) == 48);
    CHECK(perft(kiwipete, 2) == 2'039);
    const auto three = parse_fen(kCases[2].fen).value();
    CHECK(perft(three, 1) == 14);
    CHECK(perft(three, 2) == 191);
    CHECK(perft(three, 3) == 2'812);
    const auto four = parse_fen(kCases[3].fen).value();
    CHECK(perft(four, 1) == 6);
    CHECK(perft(four, 2) == 264);
    const auto five = parse_fen(kCases[4].fen).value();
    CHECK(perft(five, 1) == 44);
    CHECK(perft(five, 2) == 1'486);
    const auto six = parse_fen(kCases[5].fen).value();
    CHECK(perft(six, 1) == 46);
    CHECK(perft(six, 2) == 2'079);
}
