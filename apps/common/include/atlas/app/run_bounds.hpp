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

/// A headless run has no window to close, so it must be bounded or it never returns.
///
/// Failure: InvalidArgument explaining which flag to pass.
[[nodiscard]] Status validate_headless_bound(bool headless, std::uint64_t max_frames,
                                             std::uint64_t max_ticks);

}  // namespace atlas::app
