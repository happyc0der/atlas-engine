// SPDX-License-Identifier: GPL-3.0-or-later
//
// The third golden: a famous game's final position, hashed. Morphy against the Duke of
// Brunswick and Count Isouard, Paris, 1858 — thirty-three plies ending in mate. The moves are
// a fact about history and the final position is a fact about chess, so the hash below is a
// fact about this engine that three platforms can be asked to agree on. If a change makes this
// fail, decide whether it was meant to alter simulation results, and say so in the commit.
#include <atlas/chess/fen.hpp>
#include <atlas/chess/rules.hpp>
#include <atlas/chess/world.hpp>
#include <atlas/simulation/replay.hpp>
#include <atlas/simulation/save.hpp>

#include "chess_harness.hpp"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdio>
#include <string_view>

using atlas::chess::board_table;
using atlas::chess::Outcome;
using atlas::chess::parse_move;
using atlas::chess::Position;
using atlas::chess::Reason;
using atlas::chess::result_table;
using atlas::chess::state_table;
using atlas::chess::to_fen;
using atlas::chess::testing::ChessHarness;

namespace {

constexpr std::uint64_t kGoldenKernelSeed = 0x0C4E'5500'0000'1858ULL;

/// 1.e4 e5 2.Nf3 d6 3.d4 Bg4 4.dxe5 Bxf3 5.Qxf3 dxe5 6.Bc4 Nf6 7.Qb3 Qe7 8.Nc3 c6 9.Bg5 b5
/// 10.Nxb5 cxb5 11.Bxb5+ Nbd7 12.O-O-O Rd8 13.Rxd7 Rxd7 14.Rd1 Qe6 15.Bxd7+ Nxd7 16.Qb8+ Nxb8
/// 17.Rd8#
constexpr std::array<std::string_view, 33> kOperaGame{
    "e2e4", "e7e5", "g1f3", "d7d6", "d2d4", "c8g4", "d4e5", "g4f3", "d1f3", "d6e5", "f1c4",
    "g8f6", "f3b3", "d8e7", "b1c3", "c7c6", "c1g5", "b7b5", "c3b5", "c6b5", "c4b5", "b8d7",
    "e1c1", "a8d8", "d1d7", "d8d7", "h1d1", "e7e6", "b5d7", "f6d7", "b3b8", "d7b8", "d1d8",
};
// Black still holds a king-side castling right at the end: the king never left e8 and the
// h-rook never left h8. The plan for this milestone wrote the final FEN with "-", from memory,
// and the rules corrected it on the first run — which is the golden doing its job.
constexpr std::string_view kFinalFen = "1n1Rkb1r/p4ppp/4q3/4p1B1/4P3/8/PPP2PPP/2K5 b k - 1 17";

struct Golden {
    std::uint64_t initial = 0;
    std::uint64_t final_state = 0;
    std::uint64_t all_ticks = 0;
};

/// Play the whole game, one move per tick, and optionally record it.
[[nodiscard]] Golden play_opera(ChessHarness& h, atlas::sim::Kernel& kernel,
                                atlas::sim::ReplayRecorder* recorder = nullptr) {
    Golden golden;
    golden.initial = h.world().hash();
    atlas::Hasher over_time;
    for (const auto text : kOperaGame) {
        auto move = parse_move(text);
        REQUIRE(move.has_value());
        REQUIRE(h.submit(kernel.current_tick(), *move).has_value());
        const auto report = kernel.step();
        REQUIRE(report.has_value());
        INFO(text);
        REQUIRE(report->commands_applied == 1);
        REQUIRE(report->commands_declined == 0);
        if (recorder != nullptr) {
            REQUIRE(recorder->record_commands(report->applied_commands).has_value());
            REQUIRE(recorder->record_tick(*report).has_value());
        }
        over_time.add(report->state_hash);
        golden.final_state = report->state_hash;
    }
    golden.all_ticks = over_time.value();
    return golden;
}

}  // namespace

TEST_CASE("the Opera Game ends where history says, and hashes as recorded", "[chess][golden]") {
    ChessHarness h;
    auto kernel = h.kernel(kGoldenKernelSeed);
    const Golden golden = play_opera(h, kernel);

    const Position final_position =
        Position::from_tables(board_table(h.world(), h.ids()), state_table(h.world(), h.ids()));
    CHECK(to_fen(final_position) == kFinalFen);
    CHECK(result_table(h.world(), h.ids()).outcome == Outcome::WhiteWins);
    CHECK(result_table(h.world(), h.ids()).reason == Reason::Checkmate);
    CHECK(kernel.current_tick() == kOperaGame.size());

    std::printf("chess golden: initial=%#018llx final_state=%#018llx all_ticks=%#018llx\n",
                static_cast<unsigned long long>(golden.initial),
                static_cast<unsigned long long>(golden.final_state),
                static_cast<unsigned long long>(golden.all_ticks));
    // Recorded from the first run on macOS arm64, 2026-09-23. The Linux and Windows lanes are
    // the confirmation: this test runs in all of them and a mismatch anywhere fails that job.
    CHECK(golden.initial == 0x7CF3'0858'2112'403CULL);
    CHECK(golden.final_state == 0xBAEC'2954'2FFB'AA76ULL);
    CHECK(golden.all_ticks == 0x8862'D6E8'EEB7'FC02ULL);
}

TEST_CASE("the Opera Game is stable within a run", "[chess][golden]") {
    ChessHarness a;
    ChessHarness b;
    auto ka = a.kernel(kGoldenKernelSeed);
    auto kb = b.kernel(kGoldenKernelSeed);
    const Golden first = play_opera(a, ka);
    const Golden second = play_opera(b, kb);
    CHECK(first.initial == second.initial);
    CHECK(first.final_state == second.final_state);
    CHECK(first.all_ticks == second.all_ticks);
}

TEST_CASE("a recorded game replays through the engine with no chess in the replay",
          "[chess][golden][determinism]") {
    // The recording carries the commands and the hashes; the replay needs the rules to apply
    // them and nothing else. Every checkpoint must match, or the rules are not a pure function
    // of the position.
    ChessHarness h;
    atlas::sim::ReplayRecorder recorder(kGoldenKernelSeed, 0, h.world().hash(), 1);
    // The kernel records applied commands only when asked, so this one is built by hand
    // rather than through the harness.
    atlas::sim::Kernel recording_kernel(
        h.world(), h.schedule, h.commands,
        atlas::sim::KernelConfig{.seed = kGoldenKernelSeed, .record_applied_commands = true});
    (void)play_opera(h, recording_kernel, &recorder);
    const atlas::sim::Replay recording = recorder.take();
    REQUIRE(recording.commands.size() == kOperaGame.size());

    ChessHarness fresh;
    const auto result = atlas::sim::play(recording, fresh.world(), fresh.schedule, fresh.commands);
    REQUIRE(result.has_value());
    if (result->divergence.has_value()) {
        FAIL(result->divergence->description);
    }
    CHECK(result->matched());
    CHECK(result->ticks_run == kOperaGame.size());
    CHECK(result_table(fresh.world(), fresh.ids()).outcome == Outcome::WhiteWins);

    // And the bytes survive a round trip, as a file on disk would.
    const auto bytes = recording.to_bytes();
    REQUIRE(bytes.has_value());
    const auto restored = atlas::sim::Replay::from_bytes(*bytes);
    REQUIRE(restored.has_value());
    ChessHarness again;
    const auto replayed =
        atlas::sim::play(*restored, again.world(), again.schedule, again.commands);
    REQUIRE(replayed.has_value());
    CHECK(replayed->matched());
}

TEST_CASE("save mid-game, load elsewhere, and finish, against an uninterrupted game",
          "[chess][golden][save]") {
    ChessHarness straight;
    auto straight_kernel = straight.kernel(kGoldenKernelSeed);
    const Golden whole = play_opera(straight, straight_kernel);

    ChessHarness first;
    auto first_kernel = first.kernel(kGoldenKernelSeed);
    constexpr std::size_t kSplit = 16;
    for (std::size_t i = 0; i < kSplit; ++i) {
        REQUIRE(first.submit(first_kernel.current_tick(), parse_move(kOperaGame[i]).value())
                    .has_value());
        REQUIRE(first_kernel.step().has_value());
    }
    const auto bytes = atlas::sim::save(first.world(), first_kernel, first.commands).value();

    ChessHarness second;
    auto second_kernel = second.kernel(0);
    REQUIRE(atlas::sim::load(second.world(), second_kernel, second.commands, bytes).has_value());
    REQUIRE(second_kernel.current_tick() == kSplit);
    REQUIRE(atlas::chess::validate_world(second.world(), second.ids()).has_value());
    for (std::size_t i = kSplit; i < kOperaGame.size(); ++i) {
        REQUIRE(second.submit(second_kernel.current_tick(), parse_move(kOperaGame[i]).value())
                    .has_value());
        const auto report = second_kernel.step();
        REQUIRE(report.has_value());
        REQUIRE(report->commands_applied == 1);
    }
    CHECK(second.world().hash() == whole.final_state);
    CHECK(result_table(second.world(), second.ids()).reason == Reason::Checkmate);
}
