// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Turning bytes on disk into something the engine can use.
///
/// An importer runs on a worker thread and must therefore touch nothing that belongs to the
/// main thread: no window, no graphics device, no engine-wide state. It takes bytes and
/// produces a description plus decoded data, and whoever owns the device turns that into a
/// resource later, on the thread that is allowed to.
///
/// That split is the whole reason loading can be asynchronous at all.

#include <atlas/assets/asset_id.hpp>
#include <atlas/assets/virtual_path.hpp>
#include <atlas/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace atlas::assets {

/// A decoded image, ready to become a texture.
struct ImportedTexture {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Four channels, eight bits each, in red-green-blue-alpha order, tightly packed. One
    /// layout rather than several, because every format the engine draws with today is this
    /// one and converting at import is cheaper than branching at every use.
    std::vector<std::byte> pixels;
};

/// A compiled shader, ready to become a graphics resource.
struct ImportedShader {
    std::vector<std::byte> code;
    std::string entry_point;
    bool is_vertex_stage = true;
    std::uint32_t samplers = 0;
    std::uint32_t storage_textures = 0;
    std::uint32_t storage_buffers = 0;
    std::uint32_t uniform_buffers = 0;
};

/// Decode an image.
///
/// Thread-safe and free of engine state, so it can run on a worker.
[[nodiscard]] Result<ImportedTexture> import_texture(std::span<const std::byte> bytes,
                                                     std::string_view debug_name);

/// Limits on what will be decoded.
///
/// An image header is untrusted input, and a header claiming enormous dimensions is the
/// standard way to turn a decode into an allocation failure or worse. These bounds are
/// generous for anything real and fatal to anything absurd.
struct ImportLimits {
    std::uint32_t max_texture_dimension = 16384;
    /// Largest decoded image, in bytes. Sixteen thousand squared at four bytes a pixel is
    /// about a gigabyte, so this is the binding limit in practice.
    std::uint64_t max_texture_bytes = 256ULL * 1024 * 1024;
};

[[nodiscard]] const ImportLimits& import_limits() noexcept;

}  // namespace atlas::assets
