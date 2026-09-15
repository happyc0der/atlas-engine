// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Conversions between Atlas's render vocabulary and SDL's.
///
/// Private to the implementation. Every mapping in one place, so that replacing the backend
/// means rewriting this file and its neighbours rather than hunting conversions through the
/// renderer.

#include <atlas/rhi/descs.hpp>
#include <atlas/rhi/types.hpp>

#include <SDL3/SDL_gpu.h>

namespace atlas::rhi::detail {

[[nodiscard]] Backend backend_from_driver(const char* driver_name) noexcept;

[[nodiscard]] SDL_GPUShaderFormat to_sdl(ShaderFormat format) noexcept;
[[nodiscard]] SDL_GPUShaderStage to_sdl(ShaderStage stage) noexcept;
[[nodiscard]] SDL_GPUBufferUsageFlags to_sdl(BufferUsage usage) noexcept;
[[nodiscard]] SDL_GPUPrimitiveType to_sdl(PrimitiveTopology topology) noexcept;
[[nodiscard]] SDL_GPUVertexElementFormat to_sdl(VertexFormat format) noexcept;
[[nodiscard]] SDL_GPULoadOp to_sdl(LoadOp op) noexcept;
[[nodiscard]] SDL_GPUFilter to_sdl(Filter filter) noexcept;
[[nodiscard]] SDL_GPUSamplerAddressMode to_sdl(AddressMode mode) noexcept;

[[nodiscard]] TextureFormat from_sdl(SDL_GPUTextureFormat format) noexcept;
[[nodiscard]] SDL_GPUTextureFormat to_sdl(TextureFormat format) noexcept;

}  // namespace atlas::rhi::detail
