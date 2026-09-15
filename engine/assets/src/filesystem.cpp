// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/assets/filesystem.hpp>
#include <atlas/core/log.hpp>

#include <algorithm>
#include <format>
#include <fstream>
#include <system_error>

namespace atlas::assets {
namespace {

constexpr log::Category kAssets{"assets"};

/// Whether `candidate` is inside `root`, both already made absolute and symbolic-link-free.
///
/// Compared segment by segment rather than by string prefix: `/data/assets-private` starts
/// with `/data/assets` as text but is not inside it, and a prefix test would say otherwise.
[[nodiscard]] bool is_inside(const std::filesystem::path& root,
                             const std::filesystem::path& candidate) {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();

    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *root_it != *candidate_it) {
            return false;
        }
    }
    return true;
}

}  // namespace

Status FileSystem::mount(std::string_view name, const std::filesystem::path& root,
                         std::int32_t priority) {
    if (name.empty()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "a mount needs a name"));
    }

    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(root, error);
    if (error) {
        return std::unexpected(
            Error(ErrorCode::NotFound, std::format("cannot mount '{}' at '{}': {}", name,
                                                   root.string(), error.message())));
    }
    if (!std::filesystem::is_directory(canonical, error)) {
        return std::unexpected(Error(
            ErrorCode::InvalidArgument,
            std::format("cannot mount '{}': '{}' is not a directory", name, canonical.string())));
    }

    for (const auto& mount : m_mounts) {
        if (mount.name == name) {
            return std::unexpected(Error(ErrorCode::AlreadyExists,
                                         std::format("a mount named '{}' already exists", name)));
        }
    }

    m_mounts.push_back(Mount{.name = std::string{name}, .root = canonical, .priority = priority});

    // Descending priority, and stable so that two mounts of equal priority resolve in the
    // order they were added rather than in whatever order a sort happened to produce.
    std::ranges::stable_sort(
        m_mounts, [](const Mount& a, const Mount& b) { return a.priority > b.priority; });

    ATLAS_LOG_INFO(kAssets, "mounted '{}' at '{}' with priority {}", name, canonical.string(),
                   priority);
    return ok();
}

bool FileSystem::unmount(std::string_view name) {
    const auto removed =
        std::erase_if(m_mounts, [name](const Mount& mount) { return mount.name == name; });
    if (removed > 0) {
        ATLAS_LOG_INFO(kAssets, "unmounted '{}'", name);
    }
    return removed > 0;
}

Result<std::filesystem::path> FileSystem::resolve(const VirtualPath& path) const {
    if (m_mounts.empty()) {
        return std::unexpected(
            Error(ErrorCode::NotFound,
                  std::format("cannot resolve '{}': nothing is mounted", path.text())));
    }

    for (const auto& mount : m_mounts) {
        const std::filesystem::path candidate = mount.root / path.text();

        std::error_code error;
        auto canonical = std::filesystem::weakly_canonical(candidate, error);
        if (error) {
            continue;
        }

        // The path was built from the root, but a symbolic link along the way could still
        // lead outside it. What matters is where it ends up, so that is what is checked.
        if (!is_inside(mount.root, canonical)) {
            ATLAS_LOG_WARN(kAssets,
                           "'{}' in mount '{}' resolves to '{}', which is outside the mounted "
                           "root; refusing it",
                           path.text(), mount.name, canonical.string());
            continue;
        }

        if (std::filesystem::is_regular_file(canonical, error)) {
            return canonical;
        }
    }

    std::string tried;
    for (const auto& mount : m_mounts) {
        if (!tried.empty()) {
            tried += ", ";
        }
        tried += std::format("'{}'", mount.name);
    }

    return std::unexpected(Error(
        ErrorCode::AssetNotFound,
        std::format("'{}' was not found in any mounted root (tried {})", path.text(), tried)));
}

bool FileSystem::exists(const VirtualPath& path) const {
    return resolve(path).has_value();
}

Result<std::vector<std::byte>> FileSystem::read(const VirtualPath& path) const {
    auto resolved = resolve(path);
    if (!resolved) {
        return std::unexpected(std::move(resolved).error());
    }

    std::error_code error;
    const auto size = std::filesystem::file_size(*resolved, error);
    if (error) {
        return std::unexpected(
            Error(ErrorCode::AssetNotFound,
                  std::format("cannot size '{}': {}", path.text(), error.message())));
    }

    std::ifstream stream(*resolved, std::ios::binary);
    if (!stream) {
        return std::unexpected(
            Error(ErrorCode::PermissionDenied,
                  std::format("cannot open '{}' at '{}'", path.text(), resolved->string())));
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (size > 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): file reads are bytes.
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!stream) {
            return std::unexpected(
                Error(ErrorCode::MalformedData,
                      std::format("short read from '{}': expected {} bytes", path.text(), size)));
        }
    }
    return bytes;
}

Result<std::filesystem::file_time_type> FileSystem::modified_at(const VirtualPath& path) const {
    auto resolved = resolve(path);
    if (!resolved) {
        return std::unexpected(std::move(resolved).error());
    }

    std::error_code error;
    const auto time = std::filesystem::last_write_time(*resolved, error);
    if (error) {
        return std::unexpected(Error(ErrorCode::AssetNotFound,
                                     std::format("cannot read the modification time of '{}': {}",
                                                 path.text(), error.message())));
    }
    return time;
}

std::vector<VirtualPath> FileSystem::list(std::string_view prefix) const {
    std::vector<VirtualPath> found;

    for (const auto& mount : m_mounts) {
        const std::filesystem::path start = prefix.empty() ? mount.root : mount.root / prefix;

        std::error_code error;
        if (!std::filesystem::is_directory(start, error)) {
            continue;
        }

        for (const auto& entry : std::filesystem::recursive_directory_iterator(start, error)) {
            if (error) {
                break;
            }
            if (!entry.is_regular_file(error)) {
                continue;
            }

            const auto relative = std::filesystem::relative(entry.path(), mount.root, error);
            if (error) {
                continue;
            }

            // Back through parsing, so that anything the filesystem produced which Atlas
            // would not accept as a path is dropped here rather than surfacing later.
            auto parsed = VirtualPath::parse(relative.generic_string());
            if (parsed) {
                found.push_back(std::move(*parsed));
            }
        }
    }

    // Sorted and de-duplicated: a filesystem's enumeration order is arbitrary and may differ
    // between runs, and the same path can exist in two mounts. Anything that hashed or
    // serialised a listing would otherwise inherit that non-determinism.
    std::ranges::sort(found);
    found.erase(std::ranges::unique(found).begin(), found.end());
    return found;
}

}  // namespace atlas::assets
