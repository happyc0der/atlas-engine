// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Frame and tick timing over a rolling window, with tails.
///
/// A rolling window rather than the whole run, because the mean over ten minutes answers a
/// question nobody asked: what matters is recent behaviour. Totals are counted separately and
/// are exact. Tails are reported because tail behaviour is what makes an application feel
/// unresponsive; a mean hides exactly the frames a person notices.
///
/// Thread affinity: main thread. Allocation-free after construction, so it may be called
/// from a measured frame loop.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace atlas::app {

/// The quantiles of the recent frame times, in nanoseconds.
struct FramePercentiles {
    std::size_t samples = 0;
    std::uint64_t median = 0;
    std::uint64_t p90 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t max = 0;
};

class FrameCounters {
  public:
    /// Enough samples for stable tail estimates, small enough to stay in cache.
    static constexpr std::size_t kWindow = 4096;

    FrameCounters();

    void record(std::uint64_t frame_ns, std::uint64_t tick_ns, std::uint32_t ticks,
                std::uint32_t dropped) noexcept;

    /// The quantiles over the window. Sorts a copy, so not for a hot path; the run summary
    /// and a debug panel are what it is for.
    [[nodiscard]] FramePercentiles percentiles() const;

    /// Log the totals and the percentiles at the end of a run.
    void report() const;

    [[nodiscard]] std::uint64_t frames() const noexcept { return m_frames; }

    [[nodiscard]] std::uint64_t total_ticks() const noexcept { return m_total_ticks; }

    [[nodiscard]] std::uint64_t dropped_ticks() const noexcept { return m_dropped_ticks; }

    [[nodiscard]] std::uint64_t total_tick_ns() const noexcept { return m_total_tick_ns; }

  private:
    std::vector<std::uint64_t> m_frame_times;
    std::size_t m_next = 0;
    std::size_t m_samples = 0;
    std::uint64_t m_total_tick_ns = 0;
    std::uint64_t m_frames = 0;
    std::uint64_t m_total_ticks = 0;
    std::uint64_t m_dropped_ticks = 0;
};

}  // namespace atlas::app
