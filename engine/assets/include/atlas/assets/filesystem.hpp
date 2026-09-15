// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The virtual filesystem: where a virtual path actually is.
///
/// Roots are mounted with a priority, and a path resolves against the highest-priority root
/// that has it. That is how a modification directory shadows base content without either
/// knowing about the other, and it is why nothing above this layer ever names a real
/// directory.
///
/// Resolution is also a security boundary. A resolved path is verified to lie inside the
/// root it came from, not merely to have been built from one, because a symbolic link can
/// point anywhere and the check that matters is where the path ends up.

#include <atlas/assets/virtual_path.hpp>
#include <atlas/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::assets {

/// A mounted directory.
struct Mount {
    std::string name;
    std::filesystem::path root;
    /// Higher wins when the same virtual path exists in more than one root.
    std::int32_t priority = 0;
};

class FileSystem {
  public:
    /// Mount a directory.
    ///
    /// The directory must exist and be a directory; mounting something that is not is a
    /// configuration mistake worth reporting immediately rather than at the first missing
    /// asset.
    [[nodiscard]] Status mount(std::string_view name, const std::filesystem::path& root,
                               std::int32_t priority = 0);

    /// Remove a mount by name. Returns false if there was none.
    bool unmount(std::string_view name);

    [[nodiscard]] std::span<const Mount> mounts() const noexcept { return m_mounts; }

    /// Where a virtual path resolves, or an error saying which roots were tried.
    ///
    /// Searches mounts from highest priority to lowest and returns the first that exists.
    [[nodiscard]] Result<std::filesystem::path> resolve(const VirtualPath& path) const;

    [[nodiscard]] bool exists(const VirtualPath& path) const;

    /// Read a file whole.
    [[nodiscard]] Result<std::vector<std::byte>> read(const VirtualPath& path) const;

    /// Last modification time, for detecting that an asset changed on disk.
    [[nodiscard]] Result<std::filesystem::file_time_type>
    modified_at(const VirtualPath& path) const;

    /// Every virtual path under `prefix`, across all mounts, sorted.
    ///
    /// Sorted because a filesystem's own enumeration order is arbitrary and may differ
    /// between runs and machines, and anything that hashes or serialises a listing would
    /// inherit that. See docs/DETERMINISM.md.
    [[nodiscard]] std::vector<VirtualPath> list(std::string_view prefix = {}) const;

  private:
    /// Mounts, kept sorted by descending priority so that resolution is a simple scan.
    std::vector<Mount> m_mounts;
};

}  // namespace atlas::assets
