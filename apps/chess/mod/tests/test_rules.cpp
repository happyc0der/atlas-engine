// SPDX-License-Identifier: GPL-3.0-or-later
// The mod's rules, compiled natively and checked two ways (ADR-0023 D8).
//
// Against the published perft counts, which no code in this repository can be wrong about. And
// against `atlas::chess_sim`, the rules the game itself applies, over thousands of positions from
// games played at random: the same legal moves in every position, and the same position after
// every one of them, byte for byte. A move the mod thinks is legal and the rules decline would
// be a move the opponent submits and never plays, and the second check is what rules that out
// before a single game is played against it.
#include <atlas/chess/fen.hpp>
#include <atlas/chess/position.hpp>

#include "rules.h"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <vector>

using atlas::chess::Move;
using atlas::chess::MoveList;
using atlas::chess::parse_fen;
using atlas::chess::PieceKind;
using atlas::chess::Position;

namespace {

struct Case {
    std::string_view name;
    std::string_view fen;
    int depth;
    std::uint64_t nodes;
};

// The same six positions and counts as `apps/chess/sim/tests/test_perft.cpp`, from the Chess
// Programming Wiki's perft results.
constexpr std::array<Case, 6> kCases{{
    {"start", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 4, 197'281},
    {"kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3, 97'862},
    {"position 3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 4, 43'238},
    {"position 4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 3, 9'467},
    {"position 5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 3, 62'379},
    {"position 6", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 3,
     89'890},
}};

[[nodiscard]] cg_position to_guest(const Position& position) {
    cg_position out{};
    std::ranges::copy(position.squares, std::begin(out.squares));
    out.side = static_cast<std::uint8_t>(position.side_to_move);
    out.castling = position.castling;
    out.en_passant_file = position.en_passant_file;
    out.halfmove_clock = position.halfmove_clock;
    out.fullmove_number = position.fullmove_number;
    return out;
}

using Triple = std::tuple<int, int, int>;

[[nodiscard]] std::vector<Triple> guest_moves(const cg_position& position) {
    std::array<cg_move, CG_MAX_MOVES> moves{};
    const int count = cg_generate_legal(&position, moves.data());
    std::vector<Triple> out;
    for (int i = 0; i < count; ++i) {
        const auto& move = moves[static_cast<std::size_t>(i)];
        out.emplace_back(move.from, move.to, move.promotion);
    }
    std::ranges::sort(out);
    return out;
}

[[nodiscard]] std::vector<Triple> sim_moves(const Position& position) {
    MoveList moves;
    position.generate_legal(moves);
    std::vector<Triple> out;
    for (const Move& move : moves) {
        out.emplace_back(move.from, move.to, static_cast<int>(move.promotion));
    }
    std::ranges::sort(out);
    return out;
}

[[nodiscard]] bool same_position(const cg_position& guest, const Position& sim) {
    return std::ranges::equal(guest.squares, sim.squares) &&
           guest.side == static_cast<std::uint8_t>(sim.side_to_move) &&
           guest.castling == sim.castling && guest.en_passant_file == sim.en_passant_file &&
           guest.halfmove_clock == sim.halfmove_clock &&
           guest.fullmove_number == sim.fullmove_number;
}

/// A small counter-based generator, so the games below are the same on every run and machine.
[[nodiscard]] std::uint64_t mix(std::uint64_t x) {
    x += 0x9E37'79B9'7F4A'7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D0'49BB'1331'11EBULL;
    return x ^ (x >> 31U);
}

}  // namespace

TEST_CASE("the mod's rules match the published perft counts", "[chess][mod][perft]") {
    for (const auto& c : kCases) {
        INFO(c.name);
        const auto position = parse_fen(c.fen);
        REQUIRE(position.has_value());
        const cg_position guest = to_guest(*position);
        CHECK(cg_perft(&guest, c.depth) == c.nodes);
    }
}

TEST_CASE("the mod's rules match at every shallower depth too", "[chess][mod][perft]") {
    // A deeper count could match by two errors cancelling; the shallow ones cannot.
    const cg_position start = to_guest(parse_fen(kCases[0].fen).value());
    CHECK(cg_perft(&start, 1) == 20);
    CHECK(cg_perft(&start, 2) == 400);
    CHECK(cg_perft(&start, 3) == 8'902);
    const cg_position kiwipete = to_guest(parse_fen(kCases[1].fen).value());
    CHECK(cg_perft(&kiwipete, 1) == 48);
    CHECK(cg_perft(&kiwipete, 2) == 2'039);
}

TEST_CASE("the mod's rules agree with the game's in every position of many random games",
          "[chess][mod][differential]") {
    // Random games reach positions no curated list does: under-promotions, en passant with a
    // pinned pawn, castling with a rook that moved and came back. At each position the two must
    // produce the same legal moves, and for every one of those moves the same next position.
    constexpr int kGames = 300;
    constexpr int kMaxPlies = 160;
    std::uint64_t positions = 0;
    std::uint64_t moves_compared = 0;
    std::uint64_t checks_seen = 0;
    std::uint64_t promotions_seen = 0;

    for (int game = 0; game < kGames; ++game) {
        Position sim = Position::start();
        for (int ply = 0; ply < kMaxPlies; ++ply) {
            const cg_position guest = to_guest(sim);
            const auto expected = sim_moves(sim);
            const auto actual = guest_moves(guest);
            INFO("game " << game << " ply " << ply << " fen " << atlas::chess::to_fen(sim));
            REQUIRE(actual == expected);
            REQUIRE(cg_in_check(&guest, static_cast<int>(sim.side_to_move)) ==
                    (sim.in_check(sim.side_to_move) ? 1 : 0));
            ++positions;
            if (sim.in_check(sim.side_to_move)) {
                ++checks_seen;
            }

            MoveList legal;
            sim.generate_legal(legal);
            for (const Move& move : legal) {
                cg_position after{};
                const cg_move as_guest{.from = move.from,
                                       .to = move.to,
                                       .promotion = static_cast<std::uint8_t>(move.promotion)};
                cg_make(&guest, as_guest, &after);
                REQUIRE(same_position(after, sim.after(move)));
                ++moves_compared;
                if (move.promotion != PieceKind::None) {
                    ++promotions_seen;
                }
            }
            if (legal.empty()) {
                break;
            }
            const auto pick =
                mix((static_cast<std::uint64_t>(game) << 16U) + static_cast<std::uint64_t>(ply)) %
                legal.size();
            sim = sim.after(legal[static_cast<std::size_t>(pick)]);
            if (sim.halfmove_clock >= 100 || sim.insufficient_material()) {
                break;
            }
        }
    }

    // Not vacuous: enough positions, and the rarer rules actually reached.
    CHECK(positions > 20'000);
    CHECK(moves_compared > 500'000);
    CHECK(checks_seen > 100);
    CHECK(promotions_seen > 100);
}
