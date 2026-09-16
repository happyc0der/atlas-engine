// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Decoded assets kept on disk, so a repeated import is a read rather than a decode.
///
/// Keyed by **content**, not by path: the key is a hash of the source bytes together with the
/// importer's version, so a file that moves still hits and a file that changes cannot. The
/// importer version is part of the key so that changing what the importer produces
/// invalidates everything it produced before, rather than serving stale output that happens
/// to parse. The canonical hash's own algorithm version is folded in as well, for the same
/// reason one level down.
///
/// **A cache entry is untrusted input.** It is a file in a directory anyone can write to.
/// Every length is checked before it is used and a corrupt entry is discarded rather than
/// believed; the cost of a discarded entry is one decode, which is the cost of having no
/// cache at all.
///
/// **Thread affinity: any thread, concurrently.** Workers read and write this while decoding.
/// A store writes to a temporary name and renames it into place, so a reader never sees a
/// partial file and two workers storing the same key cannot tear it: the rename is atomic,
/// and both would be writing identical bytes.

#include <atlas/assets/importer.hpp>
#include <atlas/core/result.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

namespace atlas::assets {

/// Bumped whenever the texture importer's output changes in shape or meaning. Part of every
/// texture's cache key, so a bump invalidates every cached texture at once.
inline constexpr std::uint32_t kTextureImporterVersion = 1;

class ArtifactCache {
  public:
    /// Open, creating the directory if it is missing.
    ///
    /// Failure: `PermissionDenied` or `NotFound` when the directory cannot be created or is
    /// not writable, with the path in the message. A cache that cannot be written is refused
    /// up front rather than failing quietly on every store.
    [[nodiscard]] static Result<ArtifactCache> open(std::filesystem::path directory);

    /// The key for a source, from its content and the importer that will decode it.
    [[nodiscard]] static std::uint64_t key_for(std::span<const std::byte> source,
                                               std::uint32_t importer_version) noexcept;

    /// A cached texture for `key`, or nothing.
    ///
    /// Nothing means a miss, which is not an error. A present entry that fails validation is
    /// removed and reported as a miss too, with the reason logged once, because the caller's
    /// only sensible response to a corrupt entry is the same as to a missing one: decode.
    [[nodiscard]] std::optional<ImportedTexture> load_texture(std::uint64_t key) const;

    /// Store a texture under `key`.
    ///
    /// Failure: `IoFailure` if the temporary cannot be written or renamed. A failed store
    /// leaves no partial entry behind.
    [[nodiscard]] Status store_texture(std::uint64_t key, const ImportedTexture& texture) const;

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return m_directory; }

    /// Entries that were present and refused. For tests, and for noticing a bad disk.
    [[nodiscard]] std::uint64_t discarded() const noexcept;

  private:
    explicit ArtifactCache(std::filesystem::path directory);

    [[nodiscard]] std::filesystem::path entry_path(std::uint64_t key) const;

    std::filesystem::path m_directory;
    /// Shared across copies of the handle, since workers hold their own view of one cache.
    std::shared_ptr<std::atomic<std::uint64_t>> m_discarded;
};

}  // namespace atlas::assets
