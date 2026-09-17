// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

using atlas::Nanoseconds;
using atlas::SteadyClock;
using atlas::to_unsigned_ns;

TEST_CASE("a negative duration clamps to zero", "[core][time]") {
    // The accumulator's arithmetic is unsigned. A steady clock should never go backwards,
    // but a virtualised or misbehaving one can, and wrapping to eighteen quintillion
    // nanoseconds would be a spectacular way to find out.
    CHECK(to_unsigned_ns(Nanoseconds{-1}) == 0);
    CHECK(to_unsigned_ns(Nanoseconds{-1'000'000}) == 0);
    CHECK(to_unsigned_ns(Nanoseconds{0}) == 0);
    CHECK(to_unsigned_ns(Nanoseconds{12345}) == 12345);
}

TEST_CASE("a clock moves forward", "[core][time]") {
    const SteadyClock clock;
    const auto first = clock.elapsed();
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    const auto second = clock.elapsed();

    CHECK(second >= first);
    CHECK(second.count() > 0);
}

TEST_CASE("tick reports the interval since the previous tick", "[core][time]") {
    SteadyClock clock;
    (void)clock.tick();

    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    const auto delta = clock.tick();

    // Generous bounds: this asserts that tick measures an interval rather than total
    // elapsed time, not that the operating system schedules precisely.
    CHECK(delta >= std::chrono::milliseconds{1});
    CHECK(delta < std::chrono::seconds{5});

    // A second tick immediately after measures almost nothing, which is the property that
    // distinguishes it from elapsed().
    const auto immediate = clock.tick();
    CHECK(immediate < std::chrono::milliseconds{500});
}

TEST_CASE("reset restarts both the elapsed and the interval clocks", "[core][time]") {
    SteadyClock clock;
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    REQUIRE(clock.elapsed().count() > 0);

    clock.reset();
    CHECK(clock.elapsed() < std::chrono::milliseconds{500});
}
