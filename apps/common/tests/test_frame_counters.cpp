// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/app/frame_counters.hpp>

#include <catch2/catch_test_macros.hpp>

using atlas::app::FrameCounters;

TEST_CASE("an empty counter reports nothing", "[app][counters]") {
    const FrameCounters counters;
    const auto p = counters.percentiles();
    CHECK(p.samples == 0);
    CHECK(counters.frames() == 0);
}

TEST_CASE("totals are exact and the window is bounded", "[app][counters]") {
    FrameCounters counters;
    for (std::uint32_t i = 0; i < FrameCounters::kWindow * 2; ++i) {
        counters.record(1'000, 10, 2, 1);
    }
    CHECK(counters.frames() == FrameCounters::kWindow * 2);
    CHECK(counters.total_ticks() == FrameCounters::kWindow * 4);
    CHECK(counters.dropped_ticks() == FrameCounters::kWindow * 2);
    CHECK(counters.total_tick_ns() == FrameCounters::kWindow * 20);
    // The window keeps the most recent kWindow, not everything.
    CHECK(counters.percentiles().samples == FrameCounters::kWindow);
}

TEST_CASE("percentiles come from the sorted window", "[app][counters]") {
    // One hundred frames of 1..100 ns. The index arithmetic is the subtle part, and the
    // reason this code moved here with a test rather than staying untested in main.cpp.
    FrameCounters counters;
    for (std::uint64_t ns = 1; ns <= 100; ++ns) {
        counters.record(ns, 0, 0, 0);
    }
    const auto p = counters.percentiles();
    CHECK(p.samples == 100);
    CHECK(p.median == 50);  // index 49 of the sorted 1..100
    CHECK(p.p90 == 90);
    CHECK(p.p99 == 99);
    CHECK(p.max == 100);
}

TEST_CASE("the window is rolling, so old frames fall out", "[app][counters]") {
    FrameCounters counters;
    // Fill with slow frames, then overwrite entirely with fast ones.
    for (std::size_t i = 0; i < FrameCounters::kWindow; ++i) {
        counters.record(1'000'000, 0, 0, 0);
    }
    for (std::size_t i = 0; i < FrameCounters::kWindow; ++i) {
        counters.record(10, 0, 0, 0);
    }
    CHECK(counters.percentiles().max == 10);
}
