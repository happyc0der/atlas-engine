// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/simulation/tick_accumulator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>

#include <cstdint>
#include <numeric>
#include <vector>

using atlas::ErrorCode;
using atlas::sim::Speed;
using atlas::sim::SpeedPolicy;
using atlas::sim::TickAccumulator;
using atlas::sim::TickAccumulatorConfig;

namespace {

/// 60 Hz has no whole-nanosecond period: 1e9 / 60 is 16,666,666.67. It is the rate most
/// likely to expose drift, so it is the default here.
constexpr std::uint64_t kFrame60Hz = 16'666'667;

[[nodiscard]] TickAccumulator make(TickAccumulatorConfig config = {}) {
    auto accumulator = TickAccumulator::create(config);
    REQUIRE(accumulator.has_value());
    return std::move(*accumulator);
}

/// Run a sequence of frame durations and return the total ticks planned.
[[nodiscard]] std::uint64_t run(TickAccumulator& accumulator,
                                const std::vector<std::uint64_t>& frames) {
    std::uint64_t total = 0;
    for (const auto frame_ns : frames) {
        const auto plan = accumulator.advance(frame_ns);
        accumulator.commit(plan.ticks_to_run);
        total += plan.ticks_to_run;
    }
    return total;
}

}  // namespace

TEST_CASE("a valid configuration is accepted", "[simulation][tick]") {
    const auto accumulator = TickAccumulator::create({});
    REQUIRE(accumulator.has_value());
    CHECK(accumulator->current_tick() == 0);
    CHECK(accumulator->config().ticks_per_second == 60);
}

TEST_CASE("an invalid configuration is rejected with a reason", "[simulation][tick]") {
    SECTION("zero tick rate") {
        const auto result = TickAccumulator::create({.ticks_per_second = 0});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code() == ErrorCode::InvalidArgument);
    }

    SECTION("tick rate beyond the proven arithmetic range") {
        const auto result = TickAccumulator::create({.ticks_per_second = 1'000'000});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code() == ErrorCode::InvalidArgument);
    }

    SECTION("an odd tick rate is accepted: exactness does not depend on it") {
        // 7 Hz does not divide a second evenly, and that is fine. The accumulator counts
        // in units scaled by the tick rate, so the remainder never accumulates.
        const auto result = TickAccumulator::create({.ticks_per_second = 7});
        REQUIRE(result.has_value());
    }

    SECTION("a clamp that would never allow a tick") {
        const auto result = TickAccumulator::create({.max_ticks_per_frame = 0});
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("an unbounded batch that would never advance") {
        const auto result = TickAccumulator::create({.unbounded_batch = 0});
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("sixty frames of a sixtieth of a second produce exactly sixty ticks",
          "[simulation][tick]") {
    // The drift test. 1e9/60 is not a whole number of nanoseconds, so an implementation
    // that accumulated in nanoseconds per tick would lose or gain a tick over a second.
    auto accumulator = make();
    const std::vector<std::uint64_t> frames(60, kFrame60Hz);

    CHECK(run(accumulator, frames) == 60);
    CHECK(accumulator.current_tick() == 60);
}

TEST_CASE("drift stays bounded over many simulated seconds", "[simulation][tick]") {
    auto accumulator = make();
    constexpr int kSeconds = 600;  // ten minutes of 60 Hz frames

    const std::vector<std::uint64_t> frames(60ULL * kSeconds, kFrame60Hz);
    const std::uint64_t ticks = run(accumulator, frames);

    // Each frame is 1/3 of a nanosecond longer than a true sixtieth, so ten minutes of them
    // is about 12 microseconds of surplus: well under one tick, and never compounding.
    CHECK(ticks == 60ULL * kSeconds);
}

TEST_CASE("jittered frames summing to a second still produce sixty ticks", "[simulation][tick]") {
    auto accumulator = make();

    // Deliberately uneven, summing to exactly one second.
    std::vector<std::uint64_t> frames;
    frames.reserve(60);
    for (int i = 0; i < 59; ++i) {
        frames.push_back(i % 2 == 0 ? 10'000'000 : 23'333'334);
    }
    const std::uint64_t sum = std::accumulate(frames.begin(), frames.end(), std::uint64_t{0});
    frames.push_back(1'000'000'000 - sum);

    CHECK(run(accumulator, frames) == 60);
}

TEST_CASE("a long frame is clamped, and the surplus is dropped rather than carried",
          "[simulation][tick]") {
    auto accumulator = make();  // 60 Hz, clamp 8, max frame 250 ms

    // One second of elapsed time. The frame cap reduces it to 250 ms, which is 15 ticks
    // due; the per-frame clamp allows 8.
    const auto plan = accumulator.advance(1'000'000'000);

    CHECK(plan.ticks_to_run == 8);
    CHECK(plan.dropped_ticks == 7);
    CHECK(plan.fell_behind);

    // The dropped ticks must not reappear. If they were banked, the next frame would also
    // be saturated, and each frame would make the next one worse: the spiral.
    accumulator.commit(plan.ticks_to_run);
    const auto next = accumulator.advance(kFrame60Hz);
    CHECK(next.ticks_to_run == 1);
    CHECK(next.dropped_ticks == 0);
    CHECK_FALSE(next.fell_behind);
}

TEST_CASE("a frame longer than the cap is treated as the cap", "[simulation][tick]") {
    // A breakpoint or a suspended laptop produces an elapsed time of minutes. Simulating it
    // is never what was wanted.
    auto accumulator = make({.ticks_per_second = 10, .max_ticks_per_frame = 1000});

    const auto plan = accumulator.advance(60'000'000'000);  // a minute

    // 250 ms at 10 Hz is two and a half ticks, so two whole ticks are due, not six hundred.
    CHECK(plan.ticks_to_run == 2);
    CHECK(plan.dropped_ticks == 0);
}

TEST_CASE("pausing accumulates nothing", "[simulation][tick]") {
    auto accumulator = make();
    accumulator.set_speed(Speed::paused());

    const std::vector<std::uint64_t> frames(600, kFrame60Hz);  // ten seconds paused
    CHECK(run(accumulator, frames) == 0);
    CHECK(accumulator.current_tick() == 0);

    // Un-pausing must not release a burst, and must not claim the simulation fell behind.
    accumulator.set_speed(Speed::normal());
    const auto plan = accumulator.advance(kFrame60Hz);
    CHECK(plan.ticks_to_run == 1);
    CHECK_FALSE(plan.fell_behind);
}

TEST_CASE("a single step produces exactly one tick and then pauses", "[simulation][tick]") {
    auto accumulator = make();
    accumulator.set_speed(Speed::paused());
    accumulator.request_single_step();

    const auto stepped = accumulator.advance(kFrame60Hz);
    CHECK(stepped.ticks_to_run == 1);
    CHECK(stepped.alpha == 0.0F);
    accumulator.commit(stepped.ticks_to_run);

    CHECK(accumulator.speed().policy == SpeedPolicy::Paused);

    const auto after = accumulator.advance(kFrame60Hz);
    CHECK(after.ticks_to_run == 0);
    CHECK(accumulator.current_tick() == 1);
}

TEST_CASE("speed scales the tick count exactly", "[simulation][tick]") {
    SECTION("double speed doubles the ticks") {
        auto accumulator = make();
        accumulator.set_speed(Speed::times(2));
        // Raise the clamp so the measurement is of the speed, not of the clamp.
        const std::vector<std::uint64_t> frames(60, kFrame60Hz);
        auto fast = make({.max_ticks_per_frame = 64});
        fast.set_speed(Speed::times(2));
        CHECK(run(fast, frames) == 120);
    }

    SECTION("half speed halves the ticks") {
        auto accumulator = make();
        accumulator.set_speed(Speed::fraction(1, 2));
        const std::vector<std::uint64_t> frames(60, kFrame60Hz);
        CHECK(run(accumulator, frames) == 30);
    }

    SECTION("a third speed is exact over three seconds") {
        auto accumulator = make();
        accumulator.set_speed(Speed::fraction(1, 3));
        const std::vector<std::uint64_t> frames(180, kFrame60Hz);
        CHECK(run(accumulator, frames) == 60);
    }
}

TEST_CASE("changing speed mid-run neither loses nor invents a tick", "[simulation][tick]") {
    auto accumulator = make({.max_ticks_per_frame = 64});

    const std::vector<std::uint64_t> second(60, kFrame60Hz);
    const std::uint64_t at_normal = run(accumulator, second);

    accumulator.set_speed(Speed::times(2));
    const std::uint64_t at_double = run(accumulator, second);

    accumulator.set_speed(Speed::normal());
    const std::uint64_t back_to_normal = run(accumulator, second);

    CHECK(at_normal == 60);
    CHECK(at_double == 120);
    CHECK(back_to_normal == 60);
    CHECK(accumulator.current_tick() == 240);
}

TEST_CASE("the unbounded policy ignores elapsed time", "[simulation][tick]") {
    auto accumulator = make({.unbounded_batch = 32});
    accumulator.set_speed(Speed::unbounded());

    // Zero elapsed time, which is what a headless run reports when it never sleeps.
    const auto plan = accumulator.advance(0);
    CHECK(plan.ticks_to_run == 32);
    CHECK(plan.alpha == 1.0F);
    CHECK_FALSE(plan.fell_behind);

    // And the same for an enormous elapsed time: real time is simply not consulted.
    const auto other = accumulator.advance(999'999'999'999);
    CHECK(other.ticks_to_run == 32);
}

TEST_CASE("alpha reports the fraction of a tick that has elapsed", "[simulation][tick]") {
    auto accumulator = make({.ticks_per_second = 10});  // 100 ms per tick

    const auto quarter = accumulator.advance(25'000'000);  // 25 ms
    CHECK(quarter.ticks_to_run == 0);
    CHECK(quarter.alpha > 0.24F);
    CHECK(quarter.alpha < 0.26F);

    const auto three_quarters = accumulator.advance(50'000'000);  // 75 ms total
    CHECK(three_quarters.ticks_to_run == 0);
    CHECK(three_quarters.alpha > 0.74F);
    CHECK(three_quarters.alpha < 0.76F);

    const auto whole = accumulator.advance(25'000'000);  // 100 ms total
    CHECK(whole.ticks_to_run == 1);
    CHECK(whole.alpha < 0.001F);
}

TEST_CASE("the tick counter follows what was executed, not what was planned",
          "[simulation][tick]") {
    auto accumulator = make();

    const auto plan = accumulator.advance(1'000'000'000);
    REQUIRE(plan.ticks_to_run == 8);

    // A caller that managed only three of the eight reports three.
    accumulator.commit(3);
    CHECK(accumulator.current_tick() == 3);
}

TEST_CASE("tick length is exact for rates that divide a second", "[simulation][tick]") {
    const auto rate = GENERATE(1U, 2U, 4U, 5U, 8U, 10U, 20U, 25U, 40U, 50U, 100U, 1000U);
    auto accumulator = make({.ticks_per_second = rate});
    CHECK(accumulator.tick_length_ns() * rate == 1'000'000'000ULL);
}

TEST_CASE("a rate that does not divide a second reports a rounded length but still counts "
          "exactly",
          "[simulation][tick]") {
    // 60 Hz is the obvious case, and it is the default. A tick is really 16,666,666.67 ns.
    // The frame cap is raised here so the measurement is of the arithmetic rather than of
    // the clamp, which a different test already covers.
    auto accumulator =
        make({.ticks_per_second = 60, .max_ticks_per_frame = 1000, .max_frame_ns = 1'000'000'000});
    CHECK(accumulator.tick_length_ns() == 16'666'666);

    // The rounding is in the accessor, not in the accumulator: exactly one second of
    // elapsed time still yields exactly sixty ticks. Rounding down and multiplying would
    // have produced a shortfall of forty nanoseconds per second, or a lost tick every
    // seven months.
    const auto plan = accumulator.advance(1'000'000'000);
    CHECK(plan.ticks_to_run == 60);
}

TEST_CASE("reset clears accumulated time and the tick counter", "[simulation][tick]") {
    auto accumulator = make();
    const auto plan = accumulator.advance(kFrame60Hz * 3);
    accumulator.commit(plan.ticks_to_run);
    REQUIRE(accumulator.current_tick() > 0);

    accumulator.reset();
    CHECK(accumulator.current_tick() == 0);

    const auto after = accumulator.advance(0);
    CHECK(after.alpha == 0.0F);
}

TEST_CASE("total ticks match the closed form whenever the clamp never engages",
          "[simulation][tick]") {
    // A property rather than an example: for any sequence of frames that never saturates
    // the clamp, the ticks produced equal floor(total_elapsed * rate / 1e9).
    constexpr std::uint32_t kRate = 100;
    auto accumulator = make({.ticks_per_second = kRate, .max_ticks_per_frame = 1'000'000});

    auto seed = GENERATE(take(50, random(1U, 1'000'000U)));

    std::vector<std::uint64_t> frames;
    frames.reserve(200);
    std::uint64_t state = seed;
    for (int i = 0; i < 200; ++i) {
        // A small deterministic generator: the point is a varied sequence, reproducibly.
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        frames.push_back(1'000'000 + (state >> 40) % 9'000'000);  // 1 ms to 10 ms
    }

    const std::uint64_t elapsed = std::accumulate(frames.begin(), frames.end(), std::uint64_t{0});
    const std::uint64_t expected = elapsed * kRate / 1'000'000'000ULL;

    CHECK(run(accumulator, frames) == expected);
}
