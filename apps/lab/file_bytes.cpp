// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_bytes.hpp"

#include <atlas/simulation/save.hpp>

#include <format>
#include <fstream>
#include <random>
#include <string>

namespace atlas::lab {

Result<std::vector<std::byte>> read_file_bytes(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected(Error(
            ErrorCode::NotFound, std::format("cannot read '{}': {}", path.string(), ec.message())));
    }
    if (size > kMaxFileBytes) {
        return std::unexpected(
            Error(ErrorCode::OutOfRange, std::format("'{}' is {} bytes, over the {} byte limit",
                                                     path.string(), size, kMaxFileBytes)));
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::unexpected(
            Error(ErrorCode::IoFailure, std::format("cannot open '{}'", path.string())));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (size > 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): file reads are bytes.
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!stream) {
            return std::unexpected(
                Error(ErrorCode::IoFailure, std::format("short read from '{}'", path.string())));
        }
    }
    return bytes;
}

Status write_file_bytes_atomically(const std::filesystem::path& path,
                                   std::span<const std::byte> bytes) {
    std::random_device entropy;
    const auto temporary =
        path.parent_path() /
        std::format("{}.{:08x}.tmp", path.filename().string(), entropy() & 0xFFFF'FFFFU);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return std::unexpected(
                Error(ErrorCode::IoFailure, std::format("cannot create '{}'", temporary.string())));
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): writing raw bytes.
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            stream.close();  // Windows will not delete an open file.
            std::error_code ec;
            std::filesystem::remove(temporary, ec);
            return std::unexpected(Error(ErrorCode::IoFailure,
                                         std::format("writing '{}' failed", temporary.string())));
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return std::unexpected(
            Error(ErrorCode::IoFailure, std::format("cannot move '{}' into place: {}",
                                                    temporary.string(), ec.message())));
    }
    return ok();
}

Status save_world_to(const std::filesystem::path& path, const sim::World& world,
                     const sim::Kernel& kernel, const sim::CommandQueue& commands) {
    auto bytes = sim::save(world, kernel, commands);
    if (!bytes) {
        return std::unexpected(std::move(bytes).error().context("serialising the world"));
    }
    return write_file_bytes_atomically(path, *bytes);
}

Status load_world_from(const std::filesystem::path& path, sim::World& world, sim::Kernel& kernel,
                       sim::CommandQueue& commands) {
    auto bytes = read_file_bytes(path);
    if (!bytes) {
        return std::unexpected(std::move(bytes).error().context("reading the save"));
    }
    return sim::load(world, kernel, commands, *bytes);
}

}  // namespace atlas::lab
