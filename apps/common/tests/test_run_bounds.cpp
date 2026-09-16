// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/app/run_bounds.hpp>

#include <catch2/catch_test_macros.hpp>

using atlas::ErrorCode;
using atlas::app::clamp_ticks;
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
