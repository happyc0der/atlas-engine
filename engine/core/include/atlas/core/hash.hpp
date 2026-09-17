// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Canonical hashing.
///
/// Two quite different jobs share this file, and since M8 they no longer share an algorithm.
///
/// **Identifiers** — asset paths, table, system, command and stream names — are hashed by
/// `hash_string`, which is FNV-1a and is not going to change. Those values are computed at
/// compile time, are embedded in saves and replays as command and system identifiers, and
/// gain nothing whatever from being computed faster. FNV-1a is kept for them because it is
/// simple enough to reimplement from its specification in a few lines, which is what a value
/// that is a compatibility commitment needs.
///
/// **Bulk content** — simulation state, cache keys — is hashed by `hash_bytes` and `Hasher`,
/// which consume thirty-two bytes per step across four independent chains and finish with a
/// mixing step. FNV-1a consumes one byte per multiply and each multiply waits for the last,
/// so it ran at about 1 GB/s and was 87% of a million-cell simulation tick. The replacement
/// measures around 30 GB/s on the development machine and, because of the final mix, detects
/// change slightly *better* than what it replaces: 32.0 of 64 output bits change per input bit
/// flipped, against FNV-1a's 30.7, where 32 is ideal. `docs/PERFORMANCE.md` has the numbers,
/// the candidates that were rejected, and the trap that the unfinalised version of this scores
/// 16.6 and would have been a quiet downgrade.
///
/// `kHashAlgorithmVersion` therefore names the *pair*: FNV-1a for identifiers, the four-lane
/// hash for bulk bytes. It is stored alongside any persisted hash so that a change is detected
/// rather than mistaken for every stored value having changed at once.
///
/// One property is deliberately gone. FNV-1a could be continued from a previous value, so
/// hashing A and then B from A's result equalled hashing the concatenation, and `hash_bytes`
/// and `Hasher` both took a seed on that basis. A hash that consumes whole blocks cannot do
/// that: where the caller split the input would change the answer. `Hasher` keeps the property
/// that actually matters — its value depends on the bytes it was given and not on how they
/// were split — by holding partial blocks, which costs about 4% and is what makes composing a
/// key from several pieces meaningful. The seed parameters are removed rather than left to
/// mean something subtly different from what they used to.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace atlas {

/// Identifies the algorithm pair. Written alongside any stored hash.
///
/// Version 1 was FNV-1a for everything. Version 2 keeps FNV-1a for `hash_string` and uses the
/// four-lane block hash for bulk bytes.
inline constexpr std::uint32_t kHashAlgorithmVersion = 2;

namespace detail {

// FNV-1a, 64-bit, from the specification. Still the algorithm for identifiers, and still the
// arithmetic the block hash below is built from, so that there is one set of constants to
// reimplement rather than two.
inline constexpr std::uint64_t kFnvOffsetBasis = 0xCBF2'9CE4'8422'2325ULL;
inline constexpr std::uint64_t kFnvPrime = 0x0000'0100'0000'01B3ULL;

/// How many bytes the block hash consumes per step: four lanes of eight.
inline constexpr std::size_t kHashBlockBytes = 32;

/// Eight bytes as one word, little-endian on every machine, and usable at compile time.
///
/// Assembled from bytes rather than copied, because a byte copy is not a constant expression
/// and the identifier path has to stay constexpr. Every tier-one compiler recognises this
/// shape and emits a single load; that it still does is what `hash/lanes4` in the benchmarks
/// measures.
[[nodiscard]] constexpr std::uint64_t load_le64(std::span<const std::byte> bytes,
                                                std::size_t at) noexcept {
    std::uint64_t word = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        word |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[at + i])) << (i * 8U);
    }
    return word;
}

/// MurmurHash3's fmix64, also used by splitmix64. Five operations once per hash.
///
/// Not optional. A whole word exclusive-ored into an accumulator passes through exactly one
/// multiply, and a multiply by an odd constant diffuses upward only, so without this a flip in
/// a high bit of the final word reaches almost nothing.
[[nodiscard]] constexpr std::uint64_t mix_final(std::uint64_t value) noexcept {
    value ^= value >> 33U;
    value *= 0xFF51'AFD7'ED55'8CCDULL;
    value ^= value >> 33U;
    value *= 0xC4CE'B9FE'1A85'EC53ULL;
    value ^= value >> 33U;
    return value;
}

}  // namespace detail

/// Hash text with FNV-1a, continuing from `seed`.
///
/// The algorithm for identifiers, deliberately unchanged and deliberately not the fast one:
/// these values are compile-time constants written into saved files, and the seed composes,
/// so hashing A then B from A's result equals hashing the concatenation.
[[nodiscard]] constexpr std::uint64_t
hash_string(std::string_view text, std::uint64_t seed = detail::kFnvOffsetBasis) noexcept {
    std::uint64_t value = seed;
    for (const char character : text) {
        value ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        value *= detail::kFnvPrime;
    }
    return value;
}

/// Accumulates a hash over several values, as one stream of bytes.
///
/// Deliberately restrictive about what it will accept. Feeding it a type with padding, or one
/// whose size differs between platforms, would produce a hash that depends on the compiler
/// rather than on the data, and a stored value like that is worse than no value at all because
/// it looks trustworthy.
///
/// The value depends on the bytes added and not on how they were divided between calls:
/// `add(a).add(b)` equals `add(a ++ b)`. Partial blocks are held to make that true.
class Hasher {
  public:
    constexpr Hasher() noexcept = default;

    /// Add raw bytes.
    constexpr Hasher& add(std::span<const std::byte> bytes) noexcept {
        std::size_t at = 0;
        if (m_held > 0) {
            while (m_held < detail::kHashBlockBytes && at < bytes.size()) {
                m_buffer[m_held] = bytes[at];
                ++m_held;
                ++at;
            }
            if (m_held == detail::kHashBlockBytes) {
                consume(m_buffer, 0);
                m_held = 0;
            }
        }
        for (; bytes.size() - at >= detail::kHashBlockBytes; at += detail::kHashBlockBytes) {
            consume(bytes, at);
        }
        for (; at < bytes.size(); ++at) {
            m_buffer[m_held] = bytes[at];
            ++m_held;
        }
        return *this;
    }

    /// Add text as its bytes.
    ///
    /// This is the bulk algorithm, so it does **not** equal `hash_string` of the same text.
    /// That function is for identifiers and keeps FNV-1a; this is for content.
    constexpr Hasher& add(std::string_view text) noexcept {
        for (const char character : text) {
            push(static_cast<std::byte>(static_cast<unsigned char>(character)));
        }
        return *this;
    }

    /// Add an integer or an enumerator, byte by byte in a fixed order.
    ///
    /// Little-endian regardless of the machine, so that a hash written on one architecture
    /// matches one computed on another. Using the native layout would make a cache built on
    /// one machine appear stale on a machine of the other endianness.
    ///
    /// The width is part of the hash: a 32-bit one and a 64-bit one contribute different
    /// numbers of bytes, so two cache keys built from different-width fields cannot collide.
    template <typename T>
        requires std::is_integral_v<T> || std::is_enum_v<T>
    constexpr Hasher& add(T value) noexcept {
        if constexpr (std::is_enum_v<T>) {
            return add(static_cast<std::underlying_type_t<T>>(value));
        } else {
            using Unsigned = std::make_unsigned_t<T>;
            auto bits = static_cast<std::uint64_t>(static_cast<Unsigned>(value));
            for (std::size_t i = 0; i < sizeof(Unsigned); ++i) {
                push(static_cast<std::byte>(bits & 0xFFULL));
                bits >>= 8U;
            }
            return *this;
        }
    }

    /// Add a boolean as a single byte.
    constexpr Hasher& add(bool value) noexcept { return add(static_cast<std::uint8_t>(value)); }

    /// Add a floating-point value by its bit pattern.
    ///
    /// By bits rather than by value so that the result is exact and reproducible. Note that
    /// this makes positive and negative zero hash differently, and every kind of not-a-number
    /// hash differently again, which is the honest behaviour for a function whose purpose is
    /// detecting change.
    constexpr Hasher& add(float value) noexcept { return add(std::bit_cast<std::uint32_t>(value)); }

    constexpr Hasher& add(double value) noexcept {
        return add(std::bit_cast<std::uint64_t>(value));
    }

    /// The hash of everything added so far. Does not disturb the state, so it may be asked
    /// more than once and added to afterwards.
    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        std::uint64_t combined = detail::kFnvOffsetBasis;
        // Mixed by position rather than exclusive-ored together, or four lanes holding a
        // permutation of the same words would hash alike.
        for (const std::uint64_t lane : {m_a, m_b, m_c, m_d}) {
            combined = (combined ^ lane) * detail::kFnvPrime;
        }
        for (std::size_t i = 0; i < m_held; ++i) {
            combined ^= static_cast<std::uint64_t>(static_cast<unsigned char>(m_buffer[i]));
            combined *= detail::kFnvPrime;
        }
        return detail::mix_final(combined);
    }

  private:
    constexpr void push(std::byte byte) noexcept {
        m_buffer[m_held] = byte;
        ++m_held;
        if (m_held == detail::kHashBlockBytes) {
            consume(m_buffer, 0);
            m_held = 0;
        }
    }

    constexpr void consume(std::span<const std::byte> bytes, std::size_t at) noexcept {
        m_a = (m_a ^ detail::load_le64(bytes, at)) * detail::kFnvPrime;
        m_b = (m_b ^ detail::load_le64(bytes, at + 8)) * detail::kFnvPrime;
        m_c = (m_c ^ detail::load_le64(bytes, at + 16)) * detail::kFnvPrime;
        m_d = (m_d ^ detail::load_le64(bytes, at + 24)) * detail::kFnvPrime;
    }

    std::array<std::byte, detail::kHashBlockBytes> m_buffer{};
    std::uint64_t m_a = detail::kFnvOffsetBasis;
    std::uint64_t m_b = detail::kFnvOffsetBasis ^ 1ULL;
    std::uint64_t m_c = detail::kFnvOffsetBasis ^ 2ULL;
    std::uint64_t m_d = detail::kFnvOffsetBasis ^ 3ULL;
    std::size_t m_held = 0;
};

/// Hash a sequence of bytes with the bulk algorithm.
///
/// Equal to `Hasher{}.add(bytes).value()`, and equal however the caller would have split the
/// input. There is no seed parameter: this hash consumes whole blocks, so continuing it from a
/// previous value would make the answer depend on where the split fell. Use a `Hasher` to
/// build a value from several pieces.
[[nodiscard]] constexpr std::uint64_t hash_bytes(std::span<const std::byte> bytes) noexcept {
    return Hasher{}.add(bytes).value();
}

/// Format a hash as sixteen lowercase hexadecimal digits.
///
/// Fixed width and lowercase so that a hash in a filename or a log is the same text
/// everywhere, and sorts the way a reader expects.
[[nodiscard]] std::string to_hex(std::uint64_t value);

}  // namespace atlas
