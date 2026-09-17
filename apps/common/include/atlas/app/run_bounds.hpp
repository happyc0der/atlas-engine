// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The two rules every bounded run shares, kept in one place because getting either wrong
/// once already cost a real defect.
///
/// Thread affinity: pure functions.

#include <atlas/core/result.hpp>
#include <atlas/core/time.hpp>

#include <cstdint>

namespace atlas::app {

/// How many of the ticks the accumulator planned may actually run, given a --ticks bound.
///
/// Without this clamp, unbounded mode runs whole batches and overshoots by up to the batch
/// size, so "--ticks N" would mean exactly N when pacing against the clock and "N rounded up"
/// when not. The first integration test written for the sandbox caught exactly that: asking
/// for twenty thousand ticks ran twenty thousand and thirty-two. A benchmark dividing by N
/// would have been quietly wrong. `max_ticks` of zero means no bound.
[[nodiscard]] constexpr std::uint32_t clamp_ticks(std::uint32_t planned, Tick current,
                                                  std::uint64_t max_ticks) noexcept {
    if (max_ticks == 0) {
        return planned;
    }
    const std::uint64_t remaining = current >= max_ticks ? 0 : max_ticks - current;
    return remaining < planned ? static_cast<std::uint32_t>(remaining) : planned;
}

/// Upper bound on a single headless wait, so that a very low tick rate still checks its
/// stop conditions promptly.
inline constexpr std::uint64_t kMaxHeadlessSleepNs = 5'000'000;  // 5 ms

/// How long a headless frame should wait before the next tick is due.
///
/// With no window there is no vsync and nothing to draw, so a realtime headless run would
/// otherwise spin the processor flat out producing millions of empty frames a second to
/// deliver sixty ticks. Waiting until the next tick is due costs nothing and is what a
/// headless server would do. Unbounded mode does not call this at all: throughput is the
/// whole point there.
///
/// `alpha` is the accumulator's fraction of the way to the next tick. Clamped to
/// `kMaxHeadlessSleepNs` so a one-tick-per-second run still notices a stop condition within
/// five milliseconds. Both applications call this rather than each keeping its own copy:
/// they had drifted to different bounds, and only one of them carried the explanation.
[[nodiscard]] constexpr std::uint64_t headless_wait_ns(std::uint64_t tick_length_ns,
                                                       float alpha) noexcept {
    const auto elapsed = static_cast<std::uint64_t>(static_cast<double>(tick_length_ns) *
                                                    static_cast<double>(alpha));
    const std::uint64_t remaining = elapsed >= tick_length_ns ? 0 : tick_length_ns - elapsed;
    return remaining < kMaxHeadlessSleepNs ? remaining : kMaxHeadlessSleepNs;
}

/// A headless run has no window to close, so it must be bounded or it never returns.
///
/// Failure: InvalidArgument explaining which flag to pass.
[[nodiscard]] Status validate_headless_bound(bool headless, std::uint64_t max_frames,
                                             std::uint64_t max_ticks);

}  // namespace atlas::app
