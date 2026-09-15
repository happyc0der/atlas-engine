// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/simulation/tick_accumulator.hpp>

#include <algorithm>
#include <format>

namespace atlas::sim {

Result<TickAccumulator> TickAccumulator::create(TickAccumulatorConfig config) {
    // The bounds below are not taste, they are the overflow budget. advance() computes
    //   frame_ns * ticks_per_second * numerator
    // and with the maxima enforced here that is at most
    //   2.5e8 * 1e5 * 64 = 1.6e15,
    // comfortably inside the 1.8e19 an unsigned 64-bit integer holds, with room for the
    // carry and the accumulated remainder on top.
    if (config.ticks_per_second == 0 || config.ticks_per_second > kMaxTicksPerSecond) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     std::format("ticks_per_second must be in [1, {}], got {}",
                                                 kMaxTicksPerSecond, config.ticks_per_second)));
    }
    if (config.max_ticks_per_frame == 0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     "max_ticks_per_frame must be at least 1, or no "
                                     "interactive policy could ever advance"));
    }
    if (config.unbounded_batch == 0) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     "unbounded_batch must be at least 1, or the unbounded "
                                     "policy would never advance"));
    }
    if (config.max_frame_ns == 0) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "max_frame_ns must be at least 1"));
    }

    return TickAccumulator(config);
}

void TickAccumulator::set_speed(Speed speed) noexcept {
    ATLAS_ASSERT_MSG(speed.denominator != 0, "speed denominator must not be zero");
    ATLAS_ASSERT_MSG(speed.numerator <= kMaxSpeedNumerator,
                     "speed numerator exceeds the value the arithmetic is proven safe for");

    if (speed.denominator == 0) {
        speed.denominator = 1;
    }
    speed.numerator = std::min(speed.numerator, kMaxSpeedNumerator);

    // The carry is a remainder modulo the old denominator, so it is meaningless once the
    // denominator changes. Dropping it loses less than one nanosecond, once.
    if (speed.denominator != m_speed.denominator) {
        m_carry = 0;
    }

    m_speed = speed;

    if (speed.policy != SpeedPolicy::SingleStep) {
        m_single_step_pending = false;
    }
}

void TickAccumulator::request_single_step() noexcept {
    m_single_step_pending = true;
    m_speed.policy = SpeedPolicy::SingleStep;
}

void TickAccumulator::commit(std::uint32_t ticks_executed) noexcept {
    m_tick += ticks_executed;
}

void TickAccumulator::reset() noexcept {
    m_accumulated = 0;
    m_carry = 0;
    m_tick = 0;
    m_single_step_pending = false;
}

TickPlan TickAccumulator::advance(std::uint64_t frame_ns) {
    TickPlan plan;

    switch (m_speed.policy) {
    case SpeedPolicy::Paused:
        // Deliberately accumulates nothing. If a pause banked real time, un-pausing
        // after a minute would produce a burst that the clamp would then report as
        // falling behind, which is a lie: nothing was behind, it was paused.
        plan.alpha = static_cast<float>(m_accumulated) / static_cast<float>(kUnitsPerTick);
        return plan;

    case SpeedPolicy::SingleStep: {
        if (!m_single_step_pending) {
            plan.alpha = static_cast<float>(m_accumulated) / static_cast<float>(kUnitsPerTick);
            return plan;
        }
        m_single_step_pending = false;
        m_speed.policy = SpeedPolicy::Paused;
        // A step lands exactly on a tick boundary, so there is no partial tick left to
        // interpolate across.
        m_accumulated = 0;
        m_carry = 0;
        plan.ticks_to_run = 1;
        plan.alpha = 0.0F;
        return plan;
    }

    case SpeedPolicy::Unbounded:
        // Real time is irrelevant here: throughput is the point. Alpha is 1 because the
        // renderer, if there is one, should show the latest state rather than
        // interpolate towards a tick that has already been overtaken.
        plan.ticks_to_run = m_config.unbounded_batch;
        plan.alpha = 1.0F;
        return plan;

    case SpeedPolicy::Realtime: break;
    }

    // A frame longer than the cap is treated as the cap. The real elapsed time across a
    // breakpoint or a suspended laptop is minutes, and simulating it is never what anyone
    // wanted.
    const std::uint64_t clamped_ns = std::min(frame_ns, m_config.max_frame_ns);

    const std::uint64_t denominator = m_speed.denominator == 0 ? 1 : m_speed.denominator;
    const std::uint64_t scaled =
        (clamped_ns * m_config.ticks_per_second * m_speed.numerator) + m_carry;

    m_accumulated += scaled / denominator;
    m_carry = scaled % denominator;

    const std::uint64_t available = m_accumulated / kUnitsPerTick;
    m_accumulated %= kUnitsPerTick;

    const auto capped = std::min<std::uint64_t>(available, m_config.max_ticks_per_frame);

    plan.ticks_to_run = static_cast<std::uint32_t>(capped);
    plan.dropped_ticks = static_cast<std::uint32_t>(available - capped);
    plan.fell_behind = plan.dropped_ticks > 0;
    plan.alpha = static_cast<float>(m_accumulated) / static_cast<float>(kUnitsPerTick);

    return plan;
}

}  // namespace atlas::sim
