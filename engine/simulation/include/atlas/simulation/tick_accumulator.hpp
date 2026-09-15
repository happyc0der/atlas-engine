// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Decides how many simulation ticks to run for a given amount of elapsed real time.
///
/// This is the whole of the fixed-timestep policy, and it is deliberately the least
/// exciting code in the engine: integer arithmetic, no clock, no threads, no dependencies
/// beyond core. Reading no clock is what makes it directly testable, and being integer-only
/// is what keeps it from drifting. See docs/ARCHITECTURE.md and docs/DETERMINISM.md.
///
/// Speed is a scheduler policy, never a multiplier inside a simulation equation. Running at
/// two times speed asks for twice as many ticks; it does not make anything move faster per
/// tick. That distinction is what keeps results independent of frame rate.

#include <atlas/core/result.hpp>
#include <atlas/core/time.hpp>

#include <cstdint>

namespace atlas::sim {

/// How the scheduler converts elapsed real time into ticks.
enum class SpeedPolicy : std::uint8_t {
    /// No ticks, and no time accumulates. Un-pausing does not produce a burst.
    Paused,
    /// Exactly one tick, once, then Paused.
    SingleStep,
    /// Ticks track real time, scaled by a rational factor.
    Realtime,
    /// As many ticks as the batch size allows, ignoring real time entirely. This is the
    /// "run as fast as the machine permits" mode, and it is why the accumulator must not
    /// read a clock: in this mode there is nothing to read.
    Unbounded,
};

/// A speed setting: a policy plus a rational multiplier for Realtime.
///
/// Rational rather than floating point so that two times, one half, and one third are
/// exact. A float multiplier would reintroduce drift through the back door.
struct Speed {
    SpeedPolicy policy = SpeedPolicy::Realtime;
    std::uint32_t numerator = 1;
    std::uint32_t denominator = 1;

    [[nodiscard]] static constexpr Speed paused() noexcept {
        return Speed{.policy = SpeedPolicy::Paused, .numerator = 1, .denominator = 1};
    }

    [[nodiscard]] static constexpr Speed normal() noexcept { return Speed{}; }

    [[nodiscard]] static constexpr Speed times(std::uint32_t factor) noexcept {
        return Speed{.policy = SpeedPolicy::Realtime, .numerator = factor, .denominator = 1};
    }

    [[nodiscard]] static constexpr Speed fraction(std::uint32_t num, std::uint32_t den) noexcept {
        return Speed{.policy = SpeedPolicy::Realtime, .numerator = num, .denominator = den};
    }

    [[nodiscard]] static constexpr Speed unbounded() noexcept {
        return Speed{.policy = SpeedPolicy::Unbounded, .numerator = 1, .denominator = 1};
    }
};

struct TickAccumulatorConfig {
    /// Ticks per second of simulated time. The accumulator works in units scaled by this,
    /// so no rate has a fractional period and 60 Hz does not drift.
    std::uint32_t ticks_per_second = 60;

    /// Most ticks one call to advance() will ask for in an interactive policy. This is what
    /// stops the spiral: when a frame takes too long, the surplus is dropped rather than
    /// carried forward to make the next frame longer still.
    std::uint32_t max_ticks_per_frame = 8;

    /// Elapsed time longer than this is treated as this long. Covers the debugger-breakpoint
    /// and laptop-lid cases, where the real elapsed time is minutes and meaningless.
    std::uint64_t max_frame_ns = 250'000'000;

    /// Ticks returned per call under the Unbounded policy. Bounded so that the caller keeps
    /// the chance to handle input and report progress between batches.
    std::uint32_t unbounded_batch = 64;
};

/// What the caller should do this frame.
struct TickPlan {
    /// Ticks the caller should execute now.
    std::uint32_t ticks_to_run = 0;

    /// Ticks that were due but discarded by the catch-up clamp. Non-zero means the
    /// simulation is not keeping up with the requested speed.
    std::uint32_t dropped_ticks = 0;

    /// Convenience for `dropped_ticks > 0`.
    bool fell_behind = false;

    /// How far into the next tick the accumulator stands, in [0, 1). Renderers use it to
    /// interpolate between the last two published snapshots.
    float alpha = 0.0F;
};

class TickAccumulator {
  public:
    /// Validates the configuration. The limits are what keeps the internal arithmetic from
    /// overflowing 64 bits, so they are checked rather than assumed.
    [[nodiscard]] static Result<TickAccumulator> create(TickAccumulatorConfig config);

    /// Largest tick rate the arithmetic is proven safe for. See the note in the
    /// implementation for the overflow budget.
    static constexpr std::uint32_t kMaxTicksPerSecond = 100'000;

    /// Largest speed numerator, for the same reason.
    static constexpr std::uint32_t kMaxSpeedNumerator = 64;

    /// Convert elapsed real time into a plan. Reads no clock and has no side effect beyond
    /// the accumulator's own state.
    [[nodiscard]] TickPlan advance(std::uint64_t frame_ns);

    /// Record that `ticks_executed` ticks actually ran. Separate from advance() because a
    /// caller may execute fewer than planned, and the tick counter must reflect what
    /// happened rather than what was intended.
    void commit(std::uint32_t ticks_executed) noexcept;

    /// Change speed, keeping the fractional remainder.
    ///
    /// Changing the denominator discards the sub-nanosecond carry, because that carry is
    /// expressed in units of the old denominator. The loss is below one nanosecond and
    /// happens only at the moment of a speed change.
    void set_speed(Speed speed) noexcept;

    [[nodiscard]] Speed speed() const noexcept { return m_speed; }

    /// Ask for exactly one tick on the next advance(), then pause.
    void request_single_step() noexcept;

    [[nodiscard]] Tick current_tick() const noexcept { return m_tick; }

    [[nodiscard]] const TickAccumulatorConfig& config() const noexcept { return m_config; }

    /// Nanoseconds of simulated time one tick represents, rounded down.
    ///
    /// Only exact for rates that divide a second evenly; 60 Hz, for instance, is truly
    /// 16,666,666.67 ns and reports 16,666,666. The accumulator does not use this value and
    /// does not lose that remainder: it counts in nanoseconds scaled by the tick rate, so a
    /// tick is always a whole number of internal units. This accessor is for display and
    /// for converting tick counts into human units, never for driving the simulation.
    [[nodiscard]] std::uint64_t tick_length_ns() const noexcept {
        return kUnitsPerTick / m_config.ticks_per_second;
    }

    /// Discard accumulated time and reset the tick counter.
    void reset() noexcept;

  private:
    explicit TickAccumulator(TickAccumulatorConfig config) noexcept : m_config(config) {}

    /// One tick, in tick-scaled nanoseconds. The accumulator counts nanoseconds multiplied
    /// by the tick rate, so a tick is exactly this many units at any rate, and rates whose
    /// period is not a whole number of nanoseconds still do not drift.
    static constexpr std::uint64_t kUnitsPerTick = 1'000'000'000;

    TickAccumulatorConfig m_config{};
    Speed m_speed{};
    std::uint64_t m_accumulated = 0;  ///< Tick-scaled nanoseconds, always < kUnitsPerTick.
    std::uint64_t m_carry = 0;        ///< Remainder of the speed division, < denominator.
    Tick m_tick = 0;
    bool m_single_step_pending = false;
};

}  // namespace atlas::sim
