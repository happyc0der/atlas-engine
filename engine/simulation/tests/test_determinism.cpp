// SPDX-License-Identifier: GPL-3.0-or-later

/// \file
/// The tests M6 exists to pass.
///
/// Everything else in this module checks that a rule was followed. These check the thing the
/// rules are for: that the same inputs produce the same state, tick by tick, and that when
/// they do not the divergence is found and attributed rather than merely noticed.
///
/// What is being claimed is narrow and deliberate: the same build on the same platform.
/// Cross-compiler and cross-architecture reproduction is measured elsewhere and documented as
/// a limit, not asserted here. See docs/DETERMINISM.md.

#include <atlas/core/assert.hpp>
#include <atlas/simulation/replay.hpp>
#include <atlas/simulation/save.hpp>

#include "synthetic_scenario.hpp"
#include "synthetic_systems.hpp"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using atlas::ErrorCode;
using atlas::Tick;
using atlas::sim::check_writable;
using atlas::sim::Command;
using atlas::sim::HashCheckpoint;
using atlas::sim::Kernel;
using atlas::sim::KernelConfig;
using atlas::sim::kMaxReplaySystemHashes;
using atlas::sim::play;
using atlas::sim::Replay;
using atlas::sim::ReplayLimits;
using atlas::sim::ReplayRecorder;
using atlas::sim::SourceId;
using atlas::sim::testing::bump_payload;
using atlas::sim::testing::Harness;
using atlas::sim::testing::kBump;
using atlas::sim::testing::record_run;
using atlas::sim::testing::Scenario;

namespace {}  // namespace

TEST_CASE("the same command log replays to the same hashes", "[sim][determinism]") {
    const Replay recording = record_run(20260915, 200);
    REQUIRE(recording.tick_count == 200);
    REQUIRE_FALSE(recording.commands.empty());

    Scenario fresh;
    const auto result = play(recording, fresh.h.world, fresh.h.schedule, fresh.h.commands);
    REQUIRE(result.has_value());

    if (result->divergence.has_value()) {
        FAIL(result->divergence->description);
    }
    CHECK(result->matched());
    CHECK(result->ticks_run == 200);
    CHECK(result->checkpoints_checked == 200);
}

TEST_CASE("replaying repeatedly keeps matching", "[sim][determinism]") {
    // One successful replay could be luck in a way a hundred cannot.
    const Replay recording = record_run(7, 60);

    for (int attempt = 0; attempt < 10; ++attempt) {
        Scenario fresh;
        const auto result = play(recording, fresh.h.world, fresh.h.schedule, fresh.h.commands);
        REQUIRE(result.has_value());
        INFO("attempt " << attempt);
        CHECK(result->matched());
    }
}

TEST_CASE("a recorder refuses rather than producing a recording nothing can read",
          "[sim][determinism]") {
    // The writer could not fail and the reader could refuse, so a long enough run produced a
    // file that was written successfully and would never load again. That is a fake success
    // path, and it was found while lifting the command codec out of replay.cpp for M14.
    //
    // The limits are lowered here rather than reached honestly: tripping the real ceiling needs
    // ten million commands, which is half a gigabyte of allocation to prove a comparison.
    Scenario scenario;
    Kernel kernel(scenario.h.world, scenario.h.schedule, scenario.h.commands,
                  KernelConfig{.seed = 7, .record_applied_commands = true});

    ReplayRecorder recorder(7, 0, scenario.h.world.hash(), 1,
                            ReplayLimits{.max_commands = 4, .max_checkpoints = 1000});

    std::size_t recorded = 0;
    bool refused = false;
    for (int tick = 0; tick < 10 && !refused; ++tick) {
        REQUIRE(scenario.h.commands
                    .submit(static_cast<Tick>(tick), SourceId{1}, kBump, bump_payload(0, 1))
                    .has_value());
        const auto report = kernel.step();
        REQUIRE(report.has_value());

        const auto status = recorder.record_commands(report->applied_commands);
        if (!status) {
            refused = true;
            CHECK(status.error().code() == atlas::ErrorCode::Exhausted);
            // The message names the limit, because "recording stopped" without a number is not
            // something a person can act on.
            CHECK(status.error().message().contains("at most 4"));
            break;
        }
        recorded += report->applied_commands.size();
        REQUIRE(recorder.record_tick(*report).has_value());
    }

    CHECK(refused);
    CHECK(recorded == 4);

    // And the recording that stopped is still a recording: refusing whole rather than
    // truncating is what keeps the part already captured usable.
    const Replay partial = recorder.take();
    const auto written = partial.to_bytes();
    REQUIRE(written.has_value());
    CHECK(Replay::from_bytes(*written).has_value());
}

TEST_CASE("a recording larger than a reader accepts is refused before it is written",
          "[sim][determinism]") {
    // The belt to the recorder's braces, for a Replay assembled by hand rather than recorded.
    // Checked with the limit rather than against it: materialising ten million commands to trip
    // the real ceiling costs hundreds of megabytes to prove one comparison, so this asserts the
    // shape of the guard and the recorder's test above asserts that it fires.
    Replay replay;
    CHECK(check_writable(replay).has_value());

    replay.checkpoints.push_back(HashCheckpoint{.tick = 0, .state_hash = 1});
    replay.checkpoints.back().system_hashes.resize(kMaxReplaySystemHashes + 1);
    const auto refused = check_writable(replay);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == atlas::ErrorCode::Exhausted);
    CHECK_FALSE(replay.to_bytes().has_value());
}

TEST_CASE("a recording survives being written and read back", "[sim][determinism]") {
    // A replay that only works in the process that made it is not a replay.
    const Replay recording = record_run(1234, 80);

    const auto written = recording.to_bytes();
    REQUIRE(written.has_value());
    const auto restored = Replay::from_bytes(*written);
    REQUIRE(restored.has_value());

    CHECK(restored->seed == recording.seed);
    CHECK(restored->tick_count == recording.tick_count);
    CHECK(restored->initial_state_hash == recording.initial_state_hash);
    CHECK(restored->commands.size() == recording.commands.size());
    CHECK(restored->checkpoints.size() == recording.checkpoints.size());

    Scenario fresh;
    const auto result = play(*restored, fresh.h.world, fresh.h.schedule, fresh.h.commands);
    REQUIRE(result.has_value());
    CHECK(result->matched());
}

TEST_CASE("a replay from the wrong starting state is refused, not reported as divergence",
          "[sim][determinism]") {
    // The distinction matters: a playback from the wrong state was never a fair comparison,
    // and calling it a determinism failure would send someone hunting a bug that is not there.
    const Replay recording = record_run(5, 10);

    Scenario fresh;
    fresh.h.value_table().value[0] = 9999;

    const auto result = play(recording, fresh.h.world, fresh.h.schedule, fresh.h.commands);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("a divergence is detected and attributed to a system", "[sim][determinism]") {
    // The test that proves the machinery works. A recording is altered so its hashes no
    // longer describe the run, and the playback must find where and say which system.
    Replay recording = record_run(11, 40);
    REQUIRE(recording.checkpoints.size() == 40);

    // Corrupt the checkpoint at tick 25, including the sub-hash of the first system, as a
    // genuine divergence in that system would.
    auto& checkpoint = recording.checkpoints[25];
    checkpoint.state_hash ^= 0xDEAD'BEEFULL;
    REQUIRE_FALSE(checkpoint.system_hashes.empty());
    checkpoint.system_hashes[0].hash ^= 0xDEAD'BEEFULL;

    Scenario fresh;
    const auto result = play(recording, fresh.h.world, fresh.h.schedule, fresh.h.commands);
    REQUIRE(result.has_value());

    REQUIRE(result->divergence.has_value());
    CHECK(result->divergence->tick == 25);
    CHECK(result->divergence->expected_hash != result->divergence->actual_hash);

    REQUIRE(result->divergence->first_system.has_value());
    CHECK(*result->divergence->first_system == atlas::sim::system_id("increment values"));
    CHECK(result->divergence->description.contains("increment values"));
}

TEST_CASE("playback stops at the first divergence", "[sim][determinism]") {
    // Carrying on past it would produce pages of consequences of one cause.
    Replay recording = record_run(12, 50);
    recording.checkpoints[10].state_hash ^= 1;
    recording.checkpoints[30].state_hash ^= 1;

    Scenario fresh;
    const auto result = play(recording, fresh.h.world, fresh.h.schedule, fresh.h.commands);
    REQUIRE(result.has_value());
    REQUIRE(result->divergence.has_value());
    CHECK(result->divergence->tick == 10);
    CHECK(result->ticks_run == 11);
}

TEST_CASE("a changed command log changes the result", "[sim][determinism]") {
    // Confirms the replay is actually driven by its commands. Without this, a playback that
    // ignored the log entirely would pass every test above.
    Replay recording = record_run(13, 40);
    REQUIRE_FALSE(recording.commands.empty());

    recording.commands[0].payload = bump_payload(0, 12345);

    Scenario fresh;
    const auto result = play(recording, fresh.h.world, fresh.h.schedule, fresh.h.commands);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->matched());
}

TEST_CASE("a changed seed changes the result", "[sim][determinism]") {
    // Likewise for the seed: the scenario draws random numbers, so a different seed must
    // diverge. If it did not, the streams would not be reaching the state.
    Replay recording = record_run(14, 40);
    recording.seed += 1;

    Scenario fresh;
    const auto result = play(recording, fresh.h.world, fresh.h.schedule, fresh.h.commands);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->matched());
}

TEST_CASE("command submission order does not change the result", "[sim][determinism]") {
    // Commands are ordered by source and sequence, not by arrival, so feeding the same
    // commands in a different order must produce the same state. This is what lets two
    // machines agree without agreeing on timing.
    const auto run = [](bool reversed) {
        Scenario s;
        Kernel kernel(s.h.world, s.h.schedule, s.h.commands, KernelConfig{.seed = 3});

        std::vector<Command> log;
        for (std::uint32_t source = 1; source <= 3; ++source) {
            for (std::uint64_t i = 0; i < 4; ++i) {
                Command command;
                command.target = 2;
                command.source = SourceId{source};
                command.sequence = i;
                command.type = kBump;
                command.payload = bump_payload(static_cast<std::uint8_t>(source), 10);
                log.push_back(std::move(command));
            }
        }
        if (reversed) {
            std::ranges::reverse(log);
        }
        for (const Command& command : log) {
            REQUIRE(s.h.commands.submit_stamped(command).has_value());
        }

        REQUIRE(kernel.run(5).has_value());
        return s.h.world.hash();
    };

    CHECK(run(false) == run(true));
}

TEST_CASE("a recording that cannot be decoded is refused rather than replayed wrongly",
          "[sim][determinism]") {
    // A build without the command type cannot reproduce the run, and pretending otherwise
    // would produce a confident, wrong answer.
    const Replay recording = record_run(15, 10);

    Harness bare;
    REQUIRE(bare.schedule.finalise(bare.world).has_value());

    const auto result = play(recording, bare.world, bare.schedule, bare.commands);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("a replay with sparse checkpoints still checks them", "[sim][determinism]") {
    // Every tick is affordable in a test and not in a long run, so the interval has to work.
    Scenario s;
    Kernel kernel(s.h.world, s.h.schedule, s.h.commands,
                  KernelConfig{.seed = 16, .record_applied_commands = true});
    ReplayRecorder recorder(16, 0, s.h.world.hash(), 10);

    for (int i = 0; i < 100; ++i) {
        auto report = kernel.step();
        REQUIRE(report.has_value());
        REQUIRE(recorder.record_commands(report->applied_commands).has_value());
        REQUIRE(recorder.record_tick(*report).has_value());
    }

    const Replay recording = recorder.take();
    CHECK(recording.tick_count == 100);
    CHECK(recording.checkpoints.size() == 10);

    Scenario fresh;
    const auto result = play(recording, fresh.h.world, fresh.h.schedule, fresh.h.commands);
    REQUIRE(result.has_value());
    CHECK(result->matched());
    CHECK(result->checkpoints_checked == 10);
}

TEST_CASE("a corrupt recording is refused", "[sim][determinism]") {
    const Replay recording = record_run(17, 20);
    auto written = recording.to_bytes();
    REQUIRE(written.has_value());
    std::vector<std::byte> bytes = *std::move(written);

    SECTION("wrong magic") {
        bytes[0] = std::byte{0xFF};
        CHECK_FALSE(Replay::from_bytes(bytes).has_value());
    }

    SECTION("truncated anywhere") {
        for (std::size_t length = 0; length < bytes.size(); length += 7) {
            INFO("truncated to " << length);
            CHECK_FALSE(Replay::from_bytes(std::span<const std::byte>{bytes}.subspan(0, length))
                            .has_value());
        }
    }

    SECTION("trailing bytes") {
        bytes.push_back(std::byte{0});
        CHECK_FALSE(Replay::from_bytes(bytes).has_value());
    }
}

TEST_CASE("state hashes are stable across a save, a load and a replay", "[sim][determinism]") {
    // The three mechanisms together, because each is only useful if it agrees with the other
    // two. A run, saved midway, resumed from the save, must reach the hash the uninterrupted
    // run reached.
    Scenario direct;
    Kernel kernel(direct.h.world, direct.h.schedule, direct.h.commands, KernelConfig{.seed = 2718});
    REQUIRE(kernel.run(30).has_value());

    const auto bytes = atlas::sim::save(direct.h.world, kernel, direct.h.commands);
    REQUIRE(bytes.has_value());

    const auto rest = kernel.run(30);
    REQUIRE(rest.has_value());
    const std::uint64_t uninterrupted = rest->back().state_hash;

    Scenario resumed;
    Kernel resumed_kernel(resumed.h.world, resumed.h.schedule, resumed.h.commands, KernelConfig{});
    REQUIRE(
        atlas::sim::load(resumed.h.world, resumed_kernel, resumed.h.commands, *bytes).has_value());

    const auto after = resumed_kernel.run(30);
    REQUIRE(after.has_value());
    CHECK(after->back().state_hash == uninterrupted);
}
