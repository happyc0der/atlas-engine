// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Canonical hashing.
///
/// Two quite different jobs share this code, and it is worth being clear which is which.
///
/// Asset identifiers and cache keys need a hash that is **stable across runs and machines**,
/// because they are written to disk and compared later. `std::hash` is explicitly not that:
/// its results may differ between runs of the same program, let alone between platforms.
///
/// Simulation state hashing, arriving in M6, needs the same property for a harder reason:
/// a replay compares hashes computed on different days by different builds.
///
/// The algorithm is therefore fixed, documented, and versioned. FNV-1a is chosen for being
/// simple enough to reimplement from the specification in a few lines, which matters when
/// the value is a compatibility commitment. It is not the fastest hash available; when
/// profiling shows that matters, `kHashAlgorithmVersion` is what lets a faster one be
/// introduced without silently invalidating everything already stored.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace atlas {

/// Identifies the algorithm. Written alongside any stored hash, so that a change of
/// algorithm is detected rather than mistaken for a change of content.
inline constexpr std::uint32_t kHashAlgorithmVersion = 1;

namespace detail {

// FNV-1a, 64-bit, from the specification.
inline constexpr std::uint64_t kFnvOffsetBasis = 0xCBF2'9CE4'8422'2325ULL;
inline constexpr std::uint64_t kFnvPrime = 0x0000'0100'0000'01B3ULL;

}  // namespace detail

/// Hash a sequence of bytes, continuing from `seed`.
///
/// The seed makes composition possible: hashing A then B with the result of A gives the
/// same value as hashing the concatenation, which is what lets a streaming hasher exist.
[[nodiscard]] constexpr std::uint64_t
hash_bytes(std::span<const std::byte> bytes,
           std::uint64_t seed = detail::kFnvOffsetBasis) noexcept {
    std::uint64_t value = seed;
    for (const std::byte byte : bytes) {
        value ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
        value *= detail::kFnvPrime;
    }
    return value;
}

/// Hash text. Equivalent to hashing its bytes, and usable at compile time.
[[nodiscard]] constexpr std::uint64_t
hash_string(std::string_view text, std::uint64_t seed = detail::kFnvOffsetBasis) noexcept {
    std::uint64_t value = seed;
    for (const char character : text) {
        value ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        value *= detail::kFnvPrime;
    }
    return value;
}

/// Accumulates a hash over several values.
///
/// Deliberately restrictive about what it will accept. Feeding it a type with padding, or
/// one whose size differs between platforms, would produce a hash that depends on the
/// compiler rather than on the data, and a stored value like that is worse than no value at
/// all because it looks trustworthy.
class Hasher {
  public:
    constexpr Hasher() noexcept = default;

    constexpr explicit Hasher(std::uint64_t seed) noexcept : m_value(seed) {}

    constexpr Hasher& add(std::span<const std::byte> bytes) noexcept {
        m_value = hash_bytes(bytes, m_value);
        return *this;
    }

    constexpr Hasher& add(std::string_view text) noexcept {
        m_value = hash_string(text, m_value);
        return *this;
    }

    /// Add an integer or an enumerator, byte by byte in a fixed order.
    ///
    /// Little-endian regardless of the machine, so that a hash written on one architecture
    /// matches one computed on another. Using the native layout would make a cache built on
    /// one machine appear stale on a machine of the other endianness.
    ///
    /// The width is part of the hash: a 32-bit one and a 64-bit one produce different
    /// values, so two cache keys built from different-width fields cannot collide.
    template <typename T>
        requires std::is_integral_v<T> || std::is_enum_v<T>
    constexpr Hasher& add(T value) noexcept {
        if constexpr (std::is_enum_v<T>) {
            return add(static_cast<std::underlying_type_t<T>>(value));
        } else {
            using Unsigned = std::make_unsigned_t<T>;
            auto bits = static_cast<Unsigned>(value);
            for (std::size_t i = 0; i < sizeof(Unsigned); ++i) {
                m_value ^= static_cast<std::uint64_t>(bits) & 0xFFULL;
                m_value *= detail::kFnvPrime;
                bits = static_cast<Unsigned>(bits >> 8U);
            }
            return *this;
        }
    }

    /// Add a boolean as a single byte.
    constexpr Hasher& add(bool value) noexcept { return add(static_cast<std::uint8_t>(value)); }

    /// Add a floating-point value by its bit pattern.
    ///
    /// By bits rather than by value so that the result is exact and reproducible. Note that
    /// this makes positive and negative zero hash differently, and every kind of
    /// not-a-number hash differently again, which is the honest behaviour for a function
    /// whose purpose is detecting change.
    constexpr Hasher& add(float value) noexcept { return add(std::bit_cast<std::uint32_t>(value)); }

    constexpr Hasher& add(double value) noexcept {
        return add(std::bit_cast<std::uint64_t>(value));
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return m_value; }

  private:
    std::uint64_t m_value = detail::kFnvOffsetBasis;
};

/// Format a hash as sixteen lowercase hexadecimal digits.
///
/// Fixed width and lowercase so that a hash in a filename or a log is the same text
/// everywhere, and sorts the way a reader expects.
[[nodiscard]] std::string to_hex(std::uint64_t value);

}  // namespace atlas
