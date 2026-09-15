// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Random numbers that do not depend on who asked first.
///
/// **Counter-based, not stateful.** A value is a pure function of
/// `(seed, stream, tick, counter)`. There is no generator carrying state between ticks, no
/// ambient generator, and no global one. That is what makes a stream reproducible
/// independently of how many values every other stream consumed: with a shared stateful
/// generator, adding one call anywhere shifts every subsequent value everywhere, and a
/// replay stops matching for a reason that has nothing to do with the change.
///
/// **Streams are named.** `stream("movement")` and `stream("weather")` advance separately.
/// The name is hashed once, so opening a stream costs nothing worth avoiding.
///
/// **The tick is part of the key.** Two ticks never draw the same sequence, and a system
/// re-run for the same tick during a replay draws exactly what it drew before.
///
/// Thread affinity: a `RngStream` is a value with its own counter, so each system holds its
/// own and nothing is shared. Two streams built from the same key produce the same values,
/// which is the point rather than a hazard.

#include <atlas/core/hash.hpp>
#include <atlas/core/time.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace atlas::sim {

/// A stream's identity: the hash of its name.
enum class StreamId : std::uint32_t {};

[[nodiscard]] constexpr StreamId stream_id(std::string_view name) noexcept {
    const std::uint64_t full = hash_string(name);
    return StreamId{static_cast<std::uint32_t>((full >> 32) ^ (full & 0xFFFF'FFFFULL))};
}

/// One reproducible sequence.
///
/// Copying one copies its position, so a copy repeats what the original will produce. That
/// is deliberate: it makes a sequence inspectable without disturbing it.
class RngStream {
  public:
    constexpr RngStream(std::uint64_t seed, StreamId stream, Tick tick) noexcept
        : m_seed(seed), m_stream(static_cast<std::uint32_t>(stream)), m_tick(tick) {}

    /// The next 64 bits, advancing the counter.
    [[nodiscard]] constexpr std::uint64_t next_u64() noexcept {
        return mix(m_seed, m_stream, m_tick, m_counter++);
    }

    [[nodiscard]] constexpr std::uint32_t next_u32() noexcept {
        // The high half, because the low bits of a multiply-based mixer are the weakest.
        return static_cast<std::uint32_t>(next_u64() >> 32);
    }

    /// A value in `[0, bound)`, or zero when `bound` is zero.
    ///
    /// Unbiased. The naive remainder is not: it favours the low values by roughly
    /// `bound / 2^64`, which is invisible for small bounds and real for large ones, and
    /// "invisible" is not a property to build a reproducible simulation on. This is
    /// Lemire's method, taking the high half of a widening multiply and rejecting the one
    /// window that would skew the result.
    [[nodiscard]] constexpr std::uint64_t next_below(std::uint64_t bound) noexcept {
        if (bound == 0) {
            return 0;
        }

        auto [high, low] = widening_multiply(next_u64(), bound);

        if (low < bound) {
            // Rejection threshold. Values below it fall in the short window that would make
            // some outcomes likelier than others.
            const std::uint64_t threshold = (~bound + 1U) % bound;
            while (low < threshold) {
                const auto next = widening_multiply(next_u64(), bound);
                high = next.high;
                low = next.low;
            }
        }
        return high;
    }

    /// A value in `[low, high]`, or `low` when the range is empty or inverted.
    [[nodiscard]] constexpr std::int64_t next_in_range(std::int64_t low,
                                                       std::int64_t high) noexcept {
        if (high <= low) {
            return low;
        }
        // Span computed in unsigned arithmetic, because high - low overflows a signed
        // 64-bit integer for a range spanning most of the type.
        const auto span = static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low);
        const std::uint64_t offset = next_below(span + 1U);
        return static_cast<std::int64_t>(static_cast<std::uint64_t>(low) + offset);
    }

    /// How many values have been drawn. Part of the key, so it is worth being able to see.
    [[nodiscard]] constexpr std::uint64_t counter() const noexcept { return m_counter; }

    /// Move to an explicit position, for resuming a stream from a save.
    constexpr void set_counter(std::uint64_t counter) noexcept { m_counter = counter; }

  private:
    struct Product {
        std::uint64_t high = 0;
        std::uint64_t low = 0;
    };

    /// The full 128-bit product of two 64-bit values, in two halves.
    ///
    /// Written out in 32-bit pieces rather than using a 128-bit integer type. That type is a
    /// compiler extension which MSVC does not have, and Windows is a tier-one platform here.
    /// Optimising compilers fold this back into a single widening multiply instruction on
    /// 64-bit targets, so the portability costs nothing at run time, and it is constant
    /// expression friendly, which the intrinsics are not.
    [[nodiscard]] static constexpr Product widening_multiply(std::uint64_t a,
                                                             std::uint64_t b) noexcept {
        const std::uint64_t a_low = a & 0xFFFF'FFFFULL;
        const std::uint64_t a_high = a >> 32;
        const std::uint64_t b_low = b & 0xFFFF'FFFFULL;
        const std::uint64_t b_high = b >> 32;

        const std::uint64_t low_low = a_low * b_low;
        const std::uint64_t cross_one = a_high * b_low;
        const std::uint64_t cross_two = a_low * b_high;
        const std::uint64_t high_high = a_high * b_high;

        // The carry out of the low half has to be folded into the high half, which is the
        // part a naive two-multiply version gets wrong.
        const std::uint64_t carry =
            ((low_low >> 32) + (cross_one & 0xFFFF'FFFFULL) + (cross_two & 0xFFFF'FFFFULL)) >> 32;

        return Product{
            .high = high_high + (cross_one >> 32) + (cross_two >> 32) + carry,
            .low = low_low + (cross_one << 32) + (cross_two << 32),
        };
    }

    /// The mixing function: SplitMix64's finaliser over the combined key.
    ///
    /// Each key part is folded in with its own odd multiplier before finalising, so that two
    /// different keys do not collapse together: without that, a stream identifier of 2 at
    /// counter 3 and one of 3 at counter 2 would meet at the same sum.
    [[nodiscard]] static constexpr std::uint64_t mix(std::uint64_t seed, std::uint32_t stream,
                                                     Tick tick, std::uint64_t counter) noexcept {
        std::uint64_t z = seed;
        z += static_cast<std::uint64_t>(stream) * 0x9E37'79B9'7F4A'7C15ULL;
        z += tick * 0xBF58'476D'1CE4'E5B9ULL;
        z += counter * 0x94D0'49BB'1331'11EBULL;

        z += 0x9E37'79B9'7F4A'7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBULL;
        return z ^ (z >> 31);
    }

    std::uint64_t m_seed = 0;
    std::uint32_t m_stream = 0;
    Tick m_tick = 0;
    std::uint64_t m_counter = 0;
};

/// Opens streams for one tick from one seed.
///
/// Held by the tick context, so a system asks for a stream by name and cannot reach a
/// different tick's values by accident.
class RngStreams {
  public:
    constexpr RngStreams(std::uint64_t seed, Tick tick) noexcept : m_seed(seed), m_tick(tick) {}

    [[nodiscard]] constexpr RngStream stream(std::string_view name) const noexcept {
        return RngStream{m_seed, stream_id(name), m_tick};
    }

    [[nodiscard]] constexpr RngStream stream(StreamId id) const noexcept {
        return RngStream{m_seed, id, m_tick};
    }

    [[nodiscard]] constexpr std::uint64_t seed() const noexcept { return m_seed; }

    [[nodiscard]] constexpr Tick tick() const noexcept { return m_tick; }

  private:
    std::uint64_t m_seed = 0;
    Tick m_tick = 0;
};

}  // namespace atlas::sim
