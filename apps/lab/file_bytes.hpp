// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Bytes to and from a file, for saves and replays.
///
/// Not in core, which has no filesystem dependency and would be gaining one for two call
/// sites inside one application. Not in assets::FileSystem, which is a validated read-only
/// boundary, and a save is not an asset. Writes go through a temporary and a rename, so an
/// interrupted save leaves the previous file intact.
///
/// Thread affinity: main thread.

#include <atlas/core/result.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/kernel.hpp>
#include <atlas/simulation/world.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace atlas::lab {

/// A file larger than this is not a save this application wrote; refused before reading.
inline constexpr std::uint64_t kMaxFileBytes = std::uint64_t{256} * 1024 * 1024;

/// Failure: NotFound, OutOfRange over kMaxFileBytes, IoFailure on a short read.
[[nodiscard]] Result<std::vector<std::byte>> read_file_bytes(const std::filesystem::path& path);

/// Failure: IoFailure; on failure the destination is untouched.
[[nodiscard]] Status write_file_bytes_atomically(const std::filesystem::path& path,
                                                 std::span<const std::byte> bytes);

[[nodiscard]] Status save_world_to(const std::filesystem::path& path, const sim::World& world,
                                   const sim::Kernel& kernel, const sim::CommandQueue& commands);

/// A failed load changes nothing: sim::load has that contract, and this adds only the read.
[[nodiscard]] Status load_world_from(const std::filesystem::path& path, sim::World& world,
                                     sim::Kernel& kernel, sim::CommandQueue& commands);

}  // namespace atlas::lab
