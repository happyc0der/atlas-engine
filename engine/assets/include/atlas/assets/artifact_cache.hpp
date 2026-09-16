// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Decoded assets kept on disk, so a repeated import is a read rather than a decode.
///
/// Keyed by the source's **path, size and modification time**, plus the importer's version.
/// Not by content. The first version of this cache hashed the source bytes, which is the
/// safer key, and the measurement showed why it is the wrong one here: hashing a sixteen
/// megabyte PNG with the canonical byte-serial hash took fifteen milliseconds, against an
/// eight millisecond decode, so the warm path was slower than no cache at all. A key built
/// from three integers costs nothing, which is what lets the cached read be the whole cost.
///
/// The trade, stated plainly: a file rewritten with the same size inside the timestamp's
/// resolution serves the previous decode, and a moved file misses. That is the trade every
/// build system makes, and the second half is the harmless one. The importer version is in
/// the key so that changing what the importer produces invalidates everything it produced
/// before, and the canonical hash's algorithm version is folded in for the same reason one
/// level down.
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

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

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

    /// The key for a source, from where it is, how big it is, when it changed, and which
    /// importer will decode it. Cheap by design; see the file comment for why not content.
    [[nodiscard]] static std::uint64_t key_for(std::string_view path, std::uint64_t size,
                                               std::filesystem::file_time_type modified,
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
