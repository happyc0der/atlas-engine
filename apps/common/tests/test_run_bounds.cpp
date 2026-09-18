// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/app/run_bounds.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using atlas::ErrorCode;
using atlas::app::clamp_ticks;
using atlas::app::clamp_to_ready;
using atlas::app::validate_headless_bound;

TEST_CASE("no bound leaves the plan alone", "[app][bounds]") {
    STATIC_REQUIRE(clamp_ticks(64, 0, 0) == 64);
    STATIC_REQUIRE(clamp_ticks(64, 1'000'000, 0) == 64);
}

TEST_CASE("the plan is cut to what remains", "[app][bounds]") {
    // The exact case the first sandbox integration test caught: unbounded batches of 64,
    // twenty thousand asked for, thirty-two left. Before the clamp this ran 20032.
    STATIC_REQUIRE(clamp_ticks(64, 19'968, 20'000) == 32);
    STATIC_REQUIRE(clamp_ticks(64, 19'999, 20'000) == 1);
    STATIC_REQUIRE(clamp_ticks(8, 10, 100) == 8);
}

TEST_CASE("at or past the bound nothing runs", "[app][bounds]") {
    STATIC_REQUIRE(clamp_ticks(64, 20'000, 20'000) == 0);
    STATIC_REQUIRE(clamp_ticks(64, 20'001, 20'000) == 0);
}

TEST_CASE("a headless run must be bounded", "[app][bounds]") {
    const auto unbounded = validate_headless_bound(true, 0, 0);
    REQUIRE_FALSE(unbounded.has_value());
    CHECK(unbounded.error().code() == ErrorCode::InvalidArgument);

    CHECK(validate_headless_bound(true, 10, 0).has_value());
    CHECK(validate_headless_bound(true, 0, 10).has_value());
    // A windowed run can always be closed, so it needs no bound.
    CHECK(validate_headless_bound(false, 0, 0).has_value());
}

TEST_CASE("a headless wait never exceeds its bound", "[app][bounds]") {
    // A one-second tick at the very start of its period: the whole second is remaining, and
    // the bound is what stops a run from ignoring its stop conditions for that long.
    CHECK(atlas::app::headless_wait_ns(1'000'000'000, 0.0F) == atlas::app::kMaxHeadlessSleepNs);
}

TEST_CASE("a headless wait shrinks as the tick approaches", "[app][bounds]") {
    // A 16 ms tick, three quarters elapsed: 4 ms remains, which is under the bound and so is
    // returned whole. Waiting the bound instead would overshoot the tick.
    const std::uint64_t waited = atlas::app::headless_wait_ns(16'000'000, 0.75F);
    CHECK(waited == 4'000'000);
    CHECK(waited < atlas::app::kMaxHeadlessSleepNs);
}

TEST_CASE("a headless wait at or past the tick is zero", "[app][bounds]") {
    // Alpha reaching or passing one means the tick is already due. Subtracting without this
    // guard would wrap the unsigned remaining into an enormous wait.
    CHECK(atlas::app::headless_wait_ns(16'000'000, 1.0F) == 0);
    CHECK(atlas::app::headless_wait_ns(16'000'000, 1.5F) == 0);
}

TEST_CASE("a gate can only lower a tick count, never raise one", "[app][bounds]") {
    // The property the existing catch-up bound depends on. `check_catch_up_is_bounded` asserts
    // that a run at one tick per frame produces at least as many frames as ticks; a clamp that
    // could ever increase the count would break that for a reason unrelated to catch-up.
    constexpr atlas::Tick kUnbounded = std::numeric_limits<atlas::Tick>::max();
    for (const std::uint32_t planned : {0U, 1U, 8U, 64U}) {
        for (const atlas::Tick current : {atlas::Tick{0}, atlas::Tick{1}, atlas::Tick{1000}}) {
            for (const atlas::Tick horizon :
                 {atlas::Tick{0}, current, current + 1, current + 5, kUnbounded}) {
                const std::uint32_t allowed = clamp_to_ready(planned, current, horizon);
                INFO("planned " << planned << " current " << current << " horizon " << horizon);
                CHECK(allowed <= planned);
                // And every tick it permits is below the horizon, which is the half-open bound.
                // Stated as a conditional rather than as `current + allowed <= horizon`, which
                // is false for a horizon already behind the current tick — there `allowed` is
                // zero, and zero ticks past the horizon is no ticks past the horizon.
                if (allowed > 0) {
                    CHECK(current + allowed <= horizon);
                }
            }
        }
    }
}

TEST_CASE("an unbounded horizon changes nothing", "[app][bounds]") {
    // The solo path. A run with no peers must plan exactly what it planned before M14.
    constexpr atlas::Tick kUnbounded = std::numeric_limits<atlas::Tick>::max();
    STATIC_REQUIRE(clamp_to_ready(8, 0, kUnbounded) == 8);
    STATIC_REQUIRE(clamp_to_ready(64, 999'999, kUnbounded) == 64);
}

TEST_CASE("a horizon at the current tick permits nothing", "[app][bounds]") {
    // Half-open: the horizon is the first tick that is *not* ready, so equality means the very
    // next tick is the one being waited for.
    STATIC_REQUIRE(clamp_to_ready(8, 10, 10) == 0);
    STATIC_REQUIRE(clamp_to_ready(8, 10, 9) == 0);
    STATIC_REQUIRE(clamp_to_ready(8, 10, 11) == 1);
    STATIC_REQUIRE(clamp_to_ready(8, 10, 14) == 4);
}

TEST_CASE("a tick bound and a gate compose to whichever is lower", "[app][bounds]") {
    // Both are minima, so the order does not change the value — only which bound the reader
    // meets first. Checked from both sides so that a change to either cannot quietly start
    // winning.
    CHECK(clamp_to_ready(clamp_ticks(64, 10, 12), 10, 100) == 2);
    CHECK(clamp_ticks(clamp_to_ready(64, 10, 100), 10, 12) == 2);
    CHECK(clamp_to_ready(clamp_ticks(64, 10, 100), 10, 12) == 2);
    CHECK(clamp_ticks(clamp_to_ready(64, 10, 12), 10, 100) == 2);
}
