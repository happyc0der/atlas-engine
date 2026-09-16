// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/app/frame_counters.hpp>
#include <atlas/core/log.hpp>

#include <algorithm>

namespace atlas::app {
namespace {

constexpr log::Category kApp = log::category::kApp;

}  // namespace

FrameCounters::FrameCounters() {
    m_frame_times.resize(kWindow, 0);
}

void FrameCounters::record(std::uint64_t frame_ns, std::uint64_t tick_ns, std::uint32_t ticks,
                           std::uint32_t dropped) noexcept {
    m_frame_times[m_next] = frame_ns;
    m_next = (m_next + 1) % kWindow;
    m_samples = std::min(m_samples + 1, kWindow);

    m_total_tick_ns += tick_ns;
    m_total_ticks += ticks;
    m_dropped_ticks += dropped;
    ++m_frames;
}

FramePercentiles FrameCounters::percentiles() const {
    FramePercentiles out;
    if (m_samples == 0) {
        return out;
    }

    std::vector<std::uint64_t> samples(
        m_frame_times.begin(), m_frame_times.begin() + static_cast<std::ptrdiff_t>(m_samples));
    std::ranges::sort(samples);

    const auto at = [&samples](double quantile) {
        const auto index =
            static_cast<std::size_t>(static_cast<double>(samples.size() - 1) * quantile);
        return samples[index];
    };

    out.samples = samples.size();
    out.median = at(0.50);
    out.p90 = at(0.90);
    out.p99 = at(0.99);
    out.max = samples.back();
    return out;
}

void FrameCounters::report() const {
    if (m_frames == 0) {
        ATLAS_LOG_INFO(kApp, "no frames ran");
        return;
    }

    ATLAS_LOG_INFO(kApp, "frames={} ticks={} dropped={} tick time total us={}", m_frames,
                   m_total_ticks, m_dropped_ticks, m_total_tick_ns / 1000);

    const FramePercentiles p = percentiles();
    ATLAS_LOG_INFO(kApp, "frame time ns over the last {} frames: median={} p90={} p99={} max={}",
                   p.samples, p.median, p.p90, p.p99, p.max);

    if (m_dropped_ticks > 0) {
        ATLAS_LOG_WARN(kApp,
                       "{} ticks were dropped: the simulation did not keep up with the "
                       "requested speed",
                       m_dropped_ticks);
    }
}

}  // namespace atlas::app
