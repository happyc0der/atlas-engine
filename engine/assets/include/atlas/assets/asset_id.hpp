// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Stable identifiers for assets.
///
/// An identifier is the hash of the normalised virtual path, not of the file's location on
/// disk. That is what lets the same asset keep its identity when a modification directory
/// shadows it, when the project moves to another machine, and when it is written into a save
/// file and read back months later.
///
/// Because the value reaches disk, the hash has to be stable across runs and machines, which
/// `std::hash` explicitly is not. See atlas/core/hash.hpp.

#include <atlas/assets/virtual_path.hpp>
#include <atlas/core/hash.hpp>

#include <cstdint>
#include <string>

namespace atlas::assets {

/// What kind of thing an asset is.
///
/// Part of the identifier, so that a texture and a shader at the same path are different
/// assets rather than one asset that two importers disagree about.
enum class AssetType : std::uint8_t {
    Unknown = 0,
    Texture = 1,
    Shader = 2,
};

[[nodiscard]] std::string_view to_string(AssetType type) noexcept;

/// A stable, storable reference to an asset.
class AssetId {
  public:
    constexpr AssetId() noexcept = default;

    /// Derive an identifier from a path and a type.
    ///
    /// Constant-evaluable, so an identifier for a known asset can be a compile-time
    /// constant rather than something computed at startup.
    [[nodiscard]] static constexpr AssetId from(std::string_view normalised_path,
                                                AssetType type) noexcept {
        Hasher hasher;
        hasher.add(normalised_path).add(type);
        return AssetId{hasher.value(), type};
    }

    [[nodiscard]] static AssetId from(const VirtualPath& path, AssetType type) noexcept {
        return from(path.text(), type);
    }

    /// Rebuild an identifier from a stored value.
    ///
    /// For reading a file that recorded one. There is deliberately no validation: a hash
    /// cannot be checked without the path that produced it, so an identifier read from a
    /// file either matches an asset this run knows about or resolves to nothing, and
    /// resolving to nothing is already handled by the fallback.
    [[nodiscard]] static constexpr AssetId from_raw(std::uint64_t value, AssetType type) noexcept {
        return AssetId{value, type};
    }

    [[nodiscard]] constexpr bool valid() const noexcept { return m_value != 0; }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return m_value; }

    [[nodiscard]] constexpr AssetType type() const noexcept { return m_type; }

    /// Sixteen hexadecimal digits, for logs and for cache filenames.
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] friend constexpr bool operator==(AssetId, AssetId) noexcept = default;

    [[nodiscard]] friend constexpr auto operator<=>(AssetId a, AssetId b) noexcept {
        return a.m_value <=> b.m_value;
    }

  private:
    constexpr AssetId(std::uint64_t value, AssetType type) noexcept
        : m_value(value), m_type(type) {}

    std::uint64_t m_value = 0;
    AssetType m_type = AssetType::Unknown;
};

}  // namespace atlas::assets

/// Hashing support, so an identifier can be a key in an unordered container.
///
/// Note that iterating such a container is forbidden anywhere the order is observable; see
/// docs/DETERMINISM.md. Lookups are what this is for.
template <> struct std::hash<atlas::assets::AssetId> {
    [[nodiscard]] std::size_t operator()(atlas::assets::AssetId id) const noexcept {
        return static_cast<std::size_t>(id.value());
    }
};
