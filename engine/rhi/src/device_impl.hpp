// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The state behind Device, Frame and RenderPass.
///
/// Private to the implementation, and shared between its translation units so that neither
/// has to be read whole to follow the other. Every SDL type in the renderer is reachable
/// only from here and from the files that include it.

#include <atlas/core/handle.hpp>
#include <atlas/rhi/device.hpp>
#include <atlas/rhi/handles.hpp>
#include <atlas/rhi/types.hpp>

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::rhi {

struct BufferResource {
    SDL_GPUBuffer* buffer = nullptr;
    std::uint64_t size = 0;
    std::string debug_name;
};

struct TextureResource {
    SDL_GPUTexture* texture = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureFormat format = TextureFormat::Unknown;
    std::string debug_name;
};

struct SamplerResource {
    SDL_GPUSampler* sampler = nullptr;
    std::string debug_name;
};

struct ShaderResource {
    SDL_GPUShader* shader = nullptr;
    std::string debug_name;
};

struct PipelineResource {
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    std::string debug_name;
};

struct Device::Impl {
    SDL_GPUDevice* device = nullptr;
    SDL_Window* window = nullptr;
    Backend backend = Backend::Unknown;
    std::string backend_name;
    SDL_GPUShaderFormat shader_formats = SDL_GPU_SHADERFORMAT_INVALID;
    TextureFormat swapchain_format = TextureFormat::Unknown;

    HandlePool<BufferResource, BufferTag> buffers;
    HandlePool<TextureResource, TextureTag> textures;
    HandlePool<SamplerResource, SamplerTag> samplers;
    HandlePool<ShaderResource, ShaderTag> shaders;
    HandlePool<PipelineResource, GraphicsPipelineTag> pipelines;

    /// Set while a frame is open, so that a second begin_frame is refused rather than
    /// producing two command buffers that quietly fight over the swapchain.
    bool frame_open = false;

    /// A capture was asked for and has not been taken yet.
    bool capture_requested = false;
    std::optional<Capture> capture;
};

struct Frame::Impl {
    Device::Impl* device = nullptr;
    SDL_GPUCommandBuffer* commands = nullptr;
    SDL_GPUTexture* swapchain = nullptr;
    Extent2D extent;
    bool submitted = false;
    bool pass_open = false;
};

struct RenderPass::Impl {
    Frame::Impl* frame = nullptr;
    SDL_GPURenderPass* pass = nullptr;
    bool ended = false;
};

namespace detail {

/// Build an error from `what` plus whatever SDL last reported, clearing SDL's slot.
[[nodiscard]] Error gpu_error(ErrorCode code, std::string_view what);

/// Copy the swapchain image back to memory and store it on the device.
///
/// Submits `commands` and waits for it, because the pixels cannot be read before the copy
/// has finished. The caller must therefore not submit the command buffer itself.
[[nodiscard]] Status capture_swapchain(Device::Impl& device, SDL_GPUCommandBuffer* commands,
                                       SDL_GPUTexture* swapchain, Extent2D extent);

}  // namespace detail
}  // namespace atlas::rhi
