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

#include "synthetic_systems.hpp"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using atlas::ErrorCode;
using atlas::sim::Command;
using atlas::sim::command_type;
using atlas::sim::CommandHandler;
using atlas::sim::CommandType;
using atlas::sim::Kernel;
using atlas::sim::KernelConfig;
using atlas::sim::play;
using atlas::sim::Replay;
using atlas::sim::ReplayRecorder;
using atlas::sim::SourceId;
using atlas::sim::World;
using atlas::sim::testing::Harness;
using atlas::sim::testing::increment_values;
using atlas::sim::testing::random_into_counter;
using atlas::sim::testing::sum_into_counter;
using atlas::sim::testing::ValueTable;

namespace {

const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

const CommandType kBump = command_type("bump a row");

/// Adds a signed amount to one row, so commands visibly change the outcome.
[[nodiscard]] CommandHandler bump_handler(atlas::sim::TableId values) {
    CommandHandler handler;
    handler.validate = [](std::span<const std::byte> payload) -> atlas::Status {
        if (payload.size() != 5) {
            return std::unexpected(
                atlas::Error(ErrorCode::MalformedData, "expected a row and an amount"));
        }
        return atlas::ok();
    };
    handler.apply = [values](World& world, std::span<const std::byte> payload) {
        auto* table = dynamic_cast<ValueTable*>(world.table(values));
        if (table == nullptr) {
            return;
        }
        const auto row = std::to_integer<std::size_t>(payload[0]);
        if (row >= table->value.size()) {
            return;
        }
        std::int32_t amount = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            amount |= static_cast<std::int32_t>(std::to_integer<std::uint32_t>(payload[i + 1])
                                                << (i * 8));
        }
        table->value[row] += amount;
    };
    return handler;
}

[[nodiscard]] std::vector<std::byte> bump_payload(std::uint8_t row, std::int32_t amount) {
    const auto raw = static_cast<std::uint32_t>(amount);
    return {
        static_cast<std::byte>(row),
        static_cast<std::byte>(raw & 0xFFU),
        static_cast<std::byte>((raw >> 8) & 0xFFU),
        static_cast<std::byte>((raw >> 16) & 0xFFU),
        static_cast<std::byte>((raw >> 24) & 0xFFU),
    };
}

/// A harness wired with the usual three systems and the bump command.
struct Scenario {
    Harness h;

    explicit Scenario(std::size_t rows = 16) : h(rows) {
        REQUIRE(h.commands.register_handler(kBump, bump_handler(h.values)).has_value());
        REQUIRE(h.schedule.add(increment_values(h.values)).has_value());
        REQUIRE(h.schedule.add(sum_into_counter(h.values, h.counter)).has_value());
        REQUIRE(h.schedule.add(random_into_counter(h.counter)).has_value());
        REQUIRE(h.schedule.finalise(h.world).has_value());
    }
};

/// Run a scenario, feeding commands on a fixed pattern, and record it.
[[nodiscard]] Replay record_run(std::uint64_t seed, std::uint64_t ticks) {
    Scenario s;
    Kernel kernel(s.h.world, s.h.schedule, s.h.commands,
                  KernelConfig{.seed = seed, .record_applied_commands = true});

    ReplayRecorder recorder(seed, 0, s.h.world.hash(), 1);

    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        // Commands on a fixed pattern from two sources, deliberately submitted out of order
        // so the total order has something to do.
        if (tick % 3 == 0) {
            REQUIRE(s.h.commands
                        .submit(tick, SourceId{2}, kBump,
                                bump_payload(static_cast<std::uint8_t>(tick % 16), 5))
                        .has_value());
            REQUIRE(s.h.commands
                        .submit(tick, SourceId{1}, kBump,
                                bump_payload(static_cast<std::uint8_t>((tick + 3) % 16), -2))
                        .has_value());
        }

        auto report = kernel.step();
        REQUIRE(report.has_value());
        recorder.record_commands(report->applied_commands);
        recorder.record_tick(*report);
    }

    return recorder.take();
}

}  // namespace

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

TEST_CASE("a recording survives being written and read back", "[sim][determinism]") {
    // A replay that only works in the process that made it is not a replay.
    const Replay recording = record_run(1234, 80);

    const auto bytes = recording.to_bytes();
    const auto restored = Replay::from_bytes(bytes);
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
        recorder.record_commands(report->applied_commands);
        recorder.record_tick(*report);
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
    auto bytes = recording.to_bytes();

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
