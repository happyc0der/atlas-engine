// SPDX-License-Identifier: GPL-3.0-or-later
// The committed opponent, playing through the real rules (ADR-0023 D8).
//
// Each case loads `assets/mods/chess_opponent.wasm` — the module a player would load — into the
// engine's runtime, publishes the position the way the chess application does, polls it once a
// tick, and applies what it submits through the real `chess.move` command and kernel. What is
// checked is behaviour: that it plays legal moves, sees a free queen and a mate in one, waits its
// turn, never submits twice, and plays a whole game against itself the same way twice. And the
// budget: every tick of a full search fits in a quarter of the instructions the runtime allows.
#include <atlas/chess/fen.hpp>
#include <atlas/chess/mod_views.hpp>
#include <atlas/chess/rules.hpp>
#include <atlas/chess/world.hpp>
#include <atlas/script/limits.hpp>
#include <atlas/script/mod_host.hpp>
#include <atlas/script/runtime.hpp>
#include <atlas/simulation/kernel.hpp>
#include <atlas/simulation/turn_gate.hpp>

#include "chess_harness.hpp"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

using atlas::chess::Outcome;
using atlas::chess::parse_fen;
using atlas::chess::parse_move;
using atlas::chess::Seat;
using atlas::chess::testing::ChessHarness;
using atlas::script::ModHost;
using atlas::script::ModLimits;
using atlas::script::ModView;
using atlas::script::Runtime;

namespace {

[[nodiscard]] const std::vector<std::byte>& module_bytes() {
    static const std::vector<std::byte> kBytes = [] {
        std::ifstream file(std::filesystem::path{"assets/mods/chess_opponent.wasm"},
                           std::ios::binary);
        const std::vector<char> raw{std::istreambuf_iterator<char>(file),
                                    std::istreambuf_iterator<char>()};
        std::vector<std::byte> out(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            out[i] = static_cast<std::byte>(raw[i]);
        }
        return out;
    }();
    return kBytes;
}

[[nodiscard]] Runtime make_runtime() {
    auto made = Runtime::create();
    REQUIRE(made.has_value());
    return *std::move(made);
}

/// A game with the opponent in a seat, stepped a tick at a time the way the application will.
struct Game {
    ChessHarness chess;
    std::unique_ptr<ModHost> host;
    atlas::sim::Kernel kernel;
    atlas::sim::TurnGate mod_gate;
    std::uint8_t seat;
    std::uint64_t declined = 0;
    std::uint64_t applied = 0;

    Game(Runtime& runtime, std::string_view fen, Seat where, std::uint64_t seed = 7,
         const ModLimits& limits = {})
        : kernel(chess.kernel(seed)), seat(static_cast<std::uint8_t>(where)) {
        const auto position = parse_fen(fen);
        REQUIRE(position.has_value());
        atlas::chess::set_position(chess.world(), chess.ids(), *position);
        auto made = ModHost::create(runtime, 0, module_bytes(), "chess_opponent.wasm",
                                    {.seed = seed, .input_delay = 1}, limits);
        REQUIRE(made.has_value());
        host = *std::move(made);
        REQUIRE(host->start().has_value());
        auto& players = atlas::chess::players_table(chess.world(), chess.ids());
        if (where != Seat::Black) {
            players.white = host->id();
        }
        if (where != Seat::White) {
            players.black = host->id();
        }
    }

    /// One tick: publish the position, let the mod decide, then run the tick.
    void tick() {
        const auto view = atlas::chess::position_view(chess.world(), chess.ids());
        const std::array<std::uint8_t, 1> seat_view{seat};
        const std::array<ModView, 2> views{
            ModView{.name = "chess.position", .bytes = std::as_bytes(std::span(view))},
            ModView{.name = "chess.seat", .bytes = std::as_bytes(std::span(seat_view))},
        };
        host->set_views(views);
        REQUIRE(host->poll(kernel.current_tick(), chess.commands, mod_gate).has_value());
        const auto report = kernel.step();
        REQUIRE(report.has_value());
        declined += report->commands_declined;
        applied += report->commands_applied;
    }

    [[nodiscard]] Outcome outcome() {
        return atlas::chess::result_table(chess.world(), chess.ids()).outcome;
    }

    [[nodiscard]] std::string fen() {
        return atlas::chess::to_fen(atlas::chess::Position::from_tables(
            atlas::chess::board_table(chess.world(), chess.ids()),
            atlas::chess::state_table(chess.world(), chess.ids())));
    }

    /// Ticks until the mod has made `moves` moves, or `limit` ticks pass.
    void play(std::uint64_t moves, int limit) {
        for (int i = 0; i < limit && applied < moves && outcome() == Outcome::Ongoing; ++i) {
            tick();
        }
    }
};

[[nodiscard]] bool played(std::string_view before_fen, std::string_view after_fen,
                          std::string_view move_text) {
    auto position = parse_fen(before_fen);
    auto move = parse_move(move_text);
    REQUIRE(position.has_value());
    REQUIRE(move.has_value());
    return atlas::chess::to_fen(position->after(*move)) == after_fen;
}

constexpr std::string_view kStart = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

}  // namespace

TEST_CASE("the opponent answers with a legal move and the rules accept it", "[chess][opponent]") {
    auto runtime = make_runtime();
    Game game(runtime, kStart, Seat::White);
    game.play(1, 40);
    CHECK(game.applied == 1);
    CHECK(game.declined == 0);
    CHECK_FALSE(game.host->disabled());
    CHECK(game.fen() != kStart);
}

TEST_CASE("the opponent takes a queen left hanging", "[chess][opponent]") {
    auto runtime = make_runtime();
    constexpr std::string_view kFen = "4k3/8/8/3q4/4P3/8/8/4K3 w - - 0 1";
    Game game(runtime, kFen, Seat::White);
    game.play(1, 40);
    REQUIRE(game.applied == 1);
    CHECK(played(kFen, game.fen(), "e4d5"));
}

TEST_CASE("the opponent mates in one when it can", "[chess][opponent]") {
    auto runtime = make_runtime();
    constexpr std::string_view kFen = "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1";
    Game game(runtime, kFen, Seat::White);
    game.play(1, 40);
    REQUIRE(game.applied == 1);
    CHECK(played(kFen, game.fen(), "a1a8"));
    CHECK(game.outcome() == Outcome::WhiteWins);
}

TEST_CASE("the opponent does not take a piece that lets it be mated", "[chess][opponent]") {
    // Rook takes the free knight on d7 and leaves the back rank, and Re1 is mate. Two plies see
    // it only because a leaf where the opponent is in check asks whether it has a move at all;
    // on material alone the knight is three hundred points and the mate is invisible.
    auto runtime = make_runtime();
    constexpr std::string_view kFen = "4r1k1/3n1ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1";
    Game game(runtime, kFen, Seat::White);
    game.play(1, 40);
    REQUIRE(game.applied == 1);
    CHECK_FALSE(played(kFen, game.fen(), "d1d7"));
}

TEST_CASE("the opponent waits its turn and never submits twice for one position",
          "[chess][opponent]") {
    auto runtime = make_runtime();
    Game game(runtime, kStart, Seat::Black);

    // White to move and nobody playing white: the opponent must do nothing, however long.
    for (int i = 0; i < 30; ++i) {
        game.tick();
    }
    CHECK(game.host->stats().commands_submitted == 0);

    // Its turn now, and the position does not change after it answers — as if its move were
    // slow to land. It must answer once, not once a tick.
    REQUIRE(game.chess.submit(game.kernel.current_tick(), parse_move("e2e4").value()).has_value());
    game.tick();
    REQUIRE(game.applied == 1);
    const auto view = atlas::chess::position_view(game.chess.world(), game.chess.ids());
    const std::array<std::uint8_t, 1> seat{static_cast<std::uint8_t>(Seat::Black)};
    const std::array<ModView, 2> views{
        ModView{.name = "chess.position", .bytes = std::as_bytes(std::span(view))},
        ModView{.name = "chess.seat", .bytes = std::as_bytes(std::span(seat))},
    };
    game.host->set_views(views);
    atlas::sim::CommandQueue elsewhere;
    REQUIRE(atlas::chess::register_chess_commands(elsewhere, game.chess.ids()).has_value());
    for (atlas::Tick t = 100; t < 160; ++t) {
        REQUIRE(game.host->poll(t, elsewhere, game.mod_gate).has_value());
    }
    CHECK(game.host->stats().commands_submitted == 1);
}

TEST_CASE("the opponent plays a whole game against itself, and the same game twice",
          "[chess][opponent]") {
    // Both seats, from the start, until the game ends or a hundred plies pass. Every move it
    // submits must be one the rules accept, and two runs with the same seed must be identical:
    // the search is counted in moves, not time, so nothing about the machine can change it.
    const auto run = [] {
        auto runtime = make_runtime();
        Game game(runtime, kStart, Seat::Both, 11);
        game.play(100, 5'000);
        return std::tuple{game.fen(), game.applied, game.declined, game.host->disabled(),
                          game.chess.world().hash()};
    };
    const auto first = run();
    const auto second = run();
    INFO(std::get<0>(first));
    CHECK(std::get<1>(first) >= 20);
    CHECK(std::get<2>(first) == 0);
    CHECK_FALSE(std::get<3>(first));
    CHECK(first == second);
}

TEST_CASE("different seeds play different games", "[chess][opponent]") {
    // Equal scores are broken by the host's generator, keyed by the seed, so a person does not
    // meet the same game every time — and every peer given the same seed still meets the same
    // one. Without the draw every seed would play one game.
    const auto play_with = [](std::uint64_t seed) {
        auto runtime = make_runtime();
        Game game(runtime, kStart, Seat::Both, seed);
        game.play(16, 2'000);
        return game.fen();
    };
    const auto first = play_with(21);
    CHECK((play_with(22) != first || play_with(23) != first));
}

namespace {

// Positions that stress the search: the most legal moves any position is known to have, and one
// built for the other extreme, where each of the opponent's few moves faces eight queens and some
// hundred and twenty replies. That second kind is the expensive one, because a root move costs a
// generation and an evaluation per reply; two such positions are here, one with the opponent's
// moves few enough to finish in a tick and one with enough of them to fill the quota.
constexpr std::array<std::string_view, 7> kHeavy{{
    kStart,
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    "R6R/3Q4/1Q4Q1/4Q3/2Q4Q/Q4Q2/pp1Q4/kBNN1KB1 w - - 0 1",
    "7k/6pp/8/8/Q1Q1Q1Q1/8/Q1Q1Q1Q1/4K3 b - - 0 1",
    "7k/1pp3pp/8/8/Q1Q1Q1Q1/8/Q1Q1Q1Q1/4K3 b - - 0 1",
}};

/// Search `fen` once, as the side to move, under `limits`; whether it finished without the mod
/// being disabled.
[[nodiscard]] bool search_completes(Runtime& runtime, std::string_view fen,
                                    const ModLimits& limits) {
    const auto side = parse_fen(fen).value().side_to_move;
    Game game(runtime, fen, side == atlas::chess::Colour::White ? Seat::White : Seat::Black, 7,
              limits);
    for (int i = 0; i < 200 && game.applied == 0; ++i) {
        game.tick();
        if (game.host->disabled()) {
            return false;
        }
    }
    return game.applied == 1;
}

}  // namespace

TEST_CASE("every tick of a full search fits in a quarter of the budget", "[chess][opponent]") {
    // ADR-0023 D8's margin. A mod that exhausts its budget is disabled for the session, which
    // would leave a person's opponent silently gone; a four-fold margin is what the quota buys.
    auto runtime = make_runtime();
    const ModLimits quarter{.max_instructions_per_tick = ModLimits{}.max_instructions_per_tick / 4};
    for (const auto fen : kHeavy) {
        INFO(fen);
        // A position with no legal move for the opponent would "fail" by never answering, which
        // would be a broken test rather than a broken budget.
        atlas::chess::MoveList legal;
        parse_fen(fen).value().generate_legal(legal);
        REQUIRE_FALSE(legal.empty());
        CHECK(search_completes(runtime, fen, quarter));
    }
}

TEST_CASE("measure: the worst tick of a full search, in instructions",
          "[.measure][chess][opponent]") {
    // Hidden: run by hand, for docs/PERFORMANCE.md. The smallest budget under which a search
    // completes is the most instructions its worst tick needed, counted by the meter that
    // enforces the budget. Binary search, because the runtime reports a limit reached rather
    // than a count.
    auto runtime = make_runtime();
    std::uint32_t worst = 0;
    for (const auto fen : kHeavy) {
        std::uint32_t low = 1'000;
        std::uint32_t high = ModLimits{}.max_instructions_per_tick;
        REQUIRE(search_completes(runtime, fen, ModLimits{.max_instructions_per_tick = high}));
        while (low + 1'000 < high) {
            const std::uint32_t middle = low + ((high - low) / 2);
            if (search_completes(runtime, fen, ModLimits{.max_instructions_per_tick = middle})) {
                high = middle;
            } else {
                low = middle;
            }
        }
        std::printf("worst tick %9llu instructions  %.*s\n", static_cast<unsigned long long>(high),
                    static_cast<int>(fen.size()), fen.data());
        worst = std::max(worst, high);
    }
    std::printf("worst of all %llu instructions, budget %llu\n",
                static_cast<unsigned long long>(worst),
                static_cast<unsigned long long>(ModLimits{}.max_instructions_per_tick));
}
