// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Time types.
///
/// Atlas keeps three notions of time apart, and the type system is the first line of that
/// separation. See docs/ARCHITECTURE.md and docs/DETERMINISM.md.
///
///  - Real time drives responsiveness and frame pacing. It is nanoseconds from a steady
///    clock, and it never enters simulation state.
///  - Render time is seconds since start plus an interpolation factor.
///  - Simulation time is an integer tick counter. It is the only one that is authoritative.
///
/// A tick carries no duration. Asking how many nanoseconds a tick is worth is a question
/// for the scheduler, never for a system, which is why `Tick` is a plain counter and not a
/// `std::chrono::duration`.

#include <chrono>
#include <cstdint>

namespace atlas {

/// Nanoseconds of real time. Signed, matching std::chrono, so that differences behave.
using Nanoseconds = std::chrono::nanoseconds;

/// A simulation tick index.
///
/// Deliberately a plain 64-bit counter rather than a strong type. The original reason — that
/// this was the engine's only 64-bit counter — has expired: `Command::sequence`,
/// `Channel::published()`, `Frame::generation` and `RngStream::counter()` are all plain
/// `std::uint64_t` too. It stays plain because no mix-up between them has actually happened,
/// and a strong type here changes a signature in every module. The sharper condition, recorded
/// in docs/DEFERRED.md, is a real confusion between two of these or a serialized field that a
/// strong type would have caught.
using Tick = std::uint64_t;

/// A steady, monotonic clock.
///
/// Concrete, with no virtual interface behind it. The one component that would need a fake
/// clock to be testable, the main loop, does not exist as a testable unit yet: the tick
/// accumulator takes an elapsed duration and reads no clock at all, which is what makes it
/// directly testable. An injectable clock arrives when something needs one: a test that must
/// control time without sleeping, or a replay driven from recorded frame durations. It was
/// once tied to the runtime module, which has since been deferred with a condition that may
/// never fire, so the condition here is its own.
class SteadyClock {
  public:
    using Impl = std::chrono::steady_clock;

    /// Time since this clock was constructed.
    [[nodiscard]] Nanoseconds elapsed() const noexcept {
        return std::chrono::duration_cast<Nanoseconds>(Impl::now() - m_start);
    }

    /// Time since the previous call to tick(), or since construction for the first call.
    [[nodiscard]] Nanoseconds tick() noexcept {
        const auto now = Impl::now();
        const auto delta = std::chrono::duration_cast<Nanoseconds>(now - m_last);
        m_last = now;
        return delta;
    }

    void reset() noexcept {
        m_start = Impl::now();
        m_last = m_start;
    }

  private:
    Impl::time_point m_start = Impl::now();
    Impl::time_point m_last = m_start;
};

/// Nanoseconds as an unsigned count, which is what the tick accumulator works in.
///
/// Clamps a negative duration to zero: a steady clock should never go backwards, but a
/// virtualised or misbehaving one can, and the accumulator's arithmetic is unsigned.
[[nodiscard]] constexpr std::uint64_t to_unsigned_ns(Nanoseconds duration) noexcept {
    return duration.count() < 0 ? 0U : static_cast<std::uint64_t>(duration.count());
}

}  // namespace atlas
