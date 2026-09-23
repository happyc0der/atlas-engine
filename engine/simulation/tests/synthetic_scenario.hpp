// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// A synthetic scenario: three systems, one command that visibly changes the outcome, and a
/// helper that records a run of it.
///
/// Lifted out of `test_determinism.cpp`'s anonymous namespace in M14. It had been private to
/// that file since M6, which was right while one test needed it. The lockstep proof needs two
/// of these driving two kernels in one process, and a helper that cannot be named from another
/// translation unit is a helper the second test has to copy.
///
/// Nothing here moved except its visibility, and that is checked rather than assumed: the
/// golden scenario does not use any of it — it builds its own harness — so the recorded hashes
/// cannot be affected by this file at all.

#include <atlas/core/assert.hpp>
#include <atlas/simulation/kernel.hpp>
#include <atlas/simulation/replay.hpp>

#include "synthetic_systems.hpp"
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <vector>

namespace atlas::sim::testing {

inline const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

inline const CommandType kBump = command_type("bump a row");

/// Adds a signed amount to one row, so commands visibly change the outcome.
[[nodiscard]] inline CommandHandler bump_handler(TableId values) {
    CommandHandler handler;
    handler.validate = [](std::span<const std::byte> payload) -> Status {
        if (payload.size() != 5) {
            return std::unexpected(
                Error(atlas::ErrorCode::MalformedData, "expected a row and an amount"));
        }
        return ok();
    };
    handler.apply = [values](World& world, const ApplyContext&,
                             std::span<const std::byte> payload) -> Status {
        auto* table = dynamic_cast<ValueTable*>(world.table(values));
        // A handler registered against a table the world does not hold is a mistake in the
        // fixture, not a state the world can be in, so it is an assertion rather than a
        // decline (ADR-0019 D7).
        ATLAS_ASSERT_MSG(table != nullptr, "bump registered against a table the world lacks");
        const auto row = std::to_integer<std::size_t>(payload[0]);
        if (row >= table->value.size()) {
            // Declined: well-formed, on time, and the world has no such row. Until M19 this
            // returned silently and was counted as applied.
            return std::unexpected(
                Error(atlas::ErrorCode::OutOfRange,
                      std::format("bump names row {} of {}", row, table->value.size())));
        }
        std::int32_t amount = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            amount |= static_cast<std::int32_t>(std::to_integer<std::uint32_t>(payload[i + 1])
                                                << (i * 8));
        }
        table->value[row] += amount;
        return ok();
    };
    return handler;
}

[[nodiscard]] inline std::vector<std::byte> bump_payload(std::uint8_t row, std::int32_t amount) {
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
[[nodiscard]] inline Replay record_run(std::uint64_t seed, std::uint64_t ticks) {
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
        REQUIRE(recorder.record_commands(report->applied_commands).has_value());
        REQUIRE(recorder.record_tick(*report).has_value());
    }

    return recorder.take();
}

}  // namespace atlas::sim::testing
