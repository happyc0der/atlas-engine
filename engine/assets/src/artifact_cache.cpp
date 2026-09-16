// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/assets/artifact_cache.hpp>
#include <atlas/core/hash.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <fstream>
#include <memory>
#include <random>
#include <system_error>

namespace atlas::assets {
namespace {

constexpr log::Category kAssets{"assets"};

/// Identifies a cache entry. Recognisable in a hex dump and invalid as text.
constexpr std::uint64_t kEntryMagic = 0x45'48'43'41'43'54'41'00ULL;  // "\0ATCACHE"
constexpr std::uint32_t kEntryFormatVersion = 1;

/// Fixed-size header before the pixels. All little-endian, explicit width.
constexpr std::size_t kHeaderBytes = 8 + 4 + 4 + 4 + 4 + 8;

void put_u32(std::byte* out, std::uint32_t value) noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
        out[i] = static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
    }
}

void put_u64(std::byte* out, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<std::byte>((value >> (i * 8U)) & 0xFFULL);
    }
}

[[nodiscard]] std::uint32_t get_u32(const std::byte* in) noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint32_t>(in[i]) << (i * 8U));
    }
    return value;
}

[[nodiscard]] std::uint64_t get_u64(const std::byte* in) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint64_t>(in[i]) << (i * 8U));
    }
    return value;
}

}  // namespace

ArtifactCache::ArtifactCache(std::filesystem::path directory)
    : m_directory(std::move(directory)),
      m_discarded(std::make_shared<std::atomic<std::uint64_t>>(0)) {}

Result<ArtifactCache> ArtifactCache::open(std::filesystem::path directory) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return std::unexpected(
            Error(ErrorCode::PermissionDenied,
                  std::format("cannot create the artifact cache directory '{}': {}",
                              directory.string(), ec.message())));
    }
    if (!std::filesystem::is_directory(directory, ec)) {
        return std::unexpected(Error(
            ErrorCode::NotFound,
            std::format("the artifact cache path '{}' is not a directory", directory.string())));
    }

    // Proven writable now rather than discovered on the first store from a worker, where the
    // failure would be one line in a log per asset.
    const auto probe = directory / ".atlas-cache-probe";
    {
        const std::ofstream stream(probe, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return std::unexpected(
                Error(ErrorCode::PermissionDenied,
                      std::format("the artifact cache directory '{}' is not writable",
                                  directory.string())));
        }
    }
    std::filesystem::remove(probe, ec);

    ATLAS_LOG_INFO(kAssets, "artifact cache at '{}'", directory.string());
    return ArtifactCache(std::move(directory));
}

std::uint64_t ArtifactCache::key_for(std::string_view path, std::uint64_t size,
                                     std::filesystem::file_time_type modified,
                                     std::uint32_t importer_version) noexcept {
    // The versions go in first, so two importers reading the same file produce different
    // keys, and so a change of hash algorithm changes every key rather than colliding with an
    // entry the old algorithm wrote. The timestamp's representation is the platform's own,
    // which is fine for a cache that lives on one machine and is never shared.
    Hasher hasher;
    hasher.add(kHashAlgorithmVersion);
    hasher.add(importer_version);
    hasher.add(path);
    hasher.add(size);
    hasher.add(static_cast<std::int64_t>(modified.time_since_epoch().count()));
    return hasher.value();
}

std::filesystem::path ArtifactCache::entry_path(std::uint64_t key) const {
    return m_directory / std::format("{:016x}.texture", key);
}

std::uint64_t ArtifactCache::discarded() const noexcept {
    return m_discarded->load(std::memory_order_relaxed);
}

std::optional<ImportedTexture> ArtifactCache::load_texture(std::uint64_t key) const {
    ATLAS_ZONE_NAMED("ArtifactCache::load_texture");

    const auto path = entry_path(key);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return std::nullopt;
    }

    // Declared before the discard lambda so the lambda can close it. Windows refuses to
    // delete a file that is still open, where POSIX unlinks it regardless; removing the entry
    // with the stream open left a corrupt entry in place on Windows to be discarded again on
    // every load. Found by the Windows continuous-integration lane, not by the local tests.
    std::ifstream stream;

    // Anything wrong from here on is a corrupt entry: present, and not to be trusted.
    const auto discard = [&](std::string_view why) -> std::optional<ImportedTexture> {
        ATLAS_LOG_WARN(kAssets, "discarding cache entry '{}': {}", path.filename().string(), why);
        stream.close();
        std::error_code remove_ec;
        std::filesystem::remove(path, remove_ec);
        m_discarded->fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    };

    stream.open(path, std::ios::binary);
    if (!stream) {
        return discard("cannot be opened");
    }

    std::array<std::byte, kHeaderBytes> header{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): file reads are bytes.
    stream.read(reinterpret_cast<char*>(header.data()),
                static_cast<std::streamsize>(header.size()));
    if (stream.gcount() != static_cast<std::streamsize>(header.size())) {
        return discard("shorter than a header");
    }

    std::size_t at = 0;
    const std::uint64_t magic = get_u64(&header[at]);
    at += 8;
    if (magic != kEntryMagic) {
        return discard("wrong magic value");
    }
    const std::uint32_t format = get_u32(&header[at]);
    at += 4;
    if (format != kEntryFormatVersion) {
        return discard(
            std::format("format version {} where {} was expected", format, kEntryFormatVersion));
    }
    const std::uint32_t importer = get_u32(&header[at]);
    at += 4;
    if (importer != kTextureImporterVersion) {
        // The key already includes the importer version, so this can only happen if a key
        // collided or a file was renamed. Either way it is not this importer's output.
        return discard(std::format("importer version {} where {} was expected", importer,
                                   kTextureImporterVersion));
    }
    const std::uint32_t width = get_u32(&header[at]);
    at += 4;
    const std::uint32_t height = get_u32(&header[at]);
    at += 4;
    const std::uint64_t byte_count = get_u64(&header[at]);

    // The same bounds the importer applies to a fresh decode. A cache entry is not a way
    // around them.
    const ImportLimits& limits = import_limits();
    if (width == 0 || height == 0 || width > limits.max_texture_dimension ||
        height > limits.max_texture_dimension) {
        return discard(std::format("claims {}x{}", width, height));
    }
    const std::uint64_t expected = static_cast<std::uint64_t>(width) * height * 4;
    if (byte_count != expected) {
        return discard(std::format("claims {} pixel bytes for {}x{}, which needs {}", byte_count,
                                   width, height, expected));
    }
    if (byte_count > limits.max_texture_bytes) {
        return discard(std::format("{} bytes exceeds the decode limit", byte_count));
    }

    // Checked against the file's actual size before allocating, so a header claiming a large
    // image on a short file is refused without the allocation.
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec || file_size != kHeaderBytes + byte_count) {
        return discard(std::format("is {} bytes where {} were expected", file_size,
                                   kHeaderBytes + byte_count));
    }

    ImportedTexture texture;
    texture.width = width;
    texture.height = height;
    texture.pixels.resize(static_cast<std::size_t>(byte_count));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): file reads are bytes.
    stream.read(reinterpret_cast<char*>(texture.pixels.data()),
                static_cast<std::streamsize>(byte_count));
    if (stream.gcount() != static_cast<std::streamsize>(byte_count)) {
        return discard("ended before its pixels did");
    }

    return texture;
}

Status ArtifactCache::store_texture(std::uint64_t key, const ImportedTexture& texture) const {
    ATLAS_ZONE_NAMED("ArtifactCache::store_texture");

    const auto path = entry_path(key);

    // A temporary name unique to this writer, so two workers storing the same key do not
    // write into one another's file. The rename at the end is atomic, so a reader sees either
    // the old entry, the new one, or nothing, and never a partial one.
    static thread_local std::mt19937_64 nonce{std::random_device{}()};
    const auto temporary = m_directory / std::format("{:016x}.{:016x}.tmp", key, nonce());

    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return std::unexpected(
                Error(ErrorCode::IoFailure,
                      std::format("cannot open '{}' to write a cache entry", temporary.string())));
        }

        std::array<std::byte, kHeaderBytes> header{};
        std::size_t at = 0;
        put_u64(&header[at], kEntryMagic);
        at += 8;
        put_u32(&header[at], kEntryFormatVersion);
        at += 4;
        put_u32(&header[at], kTextureImporterVersion);
        at += 4;
        put_u32(&header[at], texture.width);
        at += 4;
        put_u32(&header[at], texture.height);
        at += 4;
        put_u64(&header[at], static_cast<std::uint64_t>(texture.pixels.size()));

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): writing raw bytes.
        stream.write(reinterpret_cast<const char*>(header.data()),
                     static_cast<std::streamsize>(header.size()));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): writing raw bytes.
        stream.write(reinterpret_cast<const char*>(texture.pixels.data()),
                     static_cast<std::streamsize>(texture.pixels.size()));
        if (!stream) {
            // Closed first for the same reason as in load_texture: Windows will not delete
            // an open file, and a failed write must not leave its temporary behind.
            stream.close();
            std::error_code ec;
            std::filesystem::remove(temporary, ec);
            return std::unexpected(
                Error(ErrorCode::IoFailure,
                      std::format("writing a cache entry to '{}' failed", temporary.string())));
        }
    }

    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return std::unexpected(
            Error(ErrorCode::IoFailure, std::format("could not move a cache entry into '{}': {}",
                                                    path.string(), ec.message())));
    }
    return ok();
}

}  // namespace atlas::assets
