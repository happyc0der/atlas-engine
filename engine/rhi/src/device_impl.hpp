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

#include "device_loss.hpp"
#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::rhi {

struct BufferResource {
    /// Staging memory for stream_buffer, created on first use and reused afterwards.
    /// Cycled on every write, so the graphics library rotates its own copies and nothing
    /// here has to track which frames are still in flight.
    SDL_GPUTransferBuffer* stream_transfer = nullptr;
    std::uint32_t stream_transfer_size = 0;

    SDL_GPUBuffer* buffer = nullptr;
    std::uint64_t size = 0;
    std::string debug_name;
};

struct TextureResource {
    SDL_GPUTexture* texture = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureFormat format = TextureFormat::Unknown;
    /// Kept so a pass can refuse a texture that was never made a colour target, with a
    /// message naming what it actually is.
    TextureUsage usage;
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

/// A readback that has been recorded and submitted and not yet collected.
struct PendingReadback {
    SDL_GPUFence* fence = nullptr;              ///< Released as soon as it signals.
    SDL_GPUTransferBuffer* transfer = nullptr;  ///< Released once the pixels are copied out.
    Rect2D region;
    TextureFormat format = TextureFormat::Unknown;
    std::uint32_t byte_count = 0;
    /// The result, with its pixel storage allocated when the readback was asked for rather
    /// than when it completes. Allocating at request time means a failure is reported through
    /// a Result, where the caller can see it; allocating at collection time would put it
    /// inside a noexcept poll, where the only options are terminating or lying.
    Device::Readback result;
    /// Whether `result` has been filled. Separate from the storage, which exists from the
    /// start.
    bool collected = false;
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

    HandlePool<PendingReadback, ReadbackTag> readbacks;

    /// Whether the graphics device is still there. See device_loss.hpp.
    detail::DeviceHealth health;

    /// Build an error for a failed graphics call, classifying it as device loss if it is.
    ///
    /// Takes the message from the graphics library, so it must be called while that message
    /// is still the one belonging to the failure being reported.
    [[nodiscard]] Error fail(ErrorCode fallback, std::string_view what);

    /// The error to return when the device is already known to be gone.
    ///
    /// Every entry point checks this first, so that one dead device produces one report
    /// rather than a different-looking failure from every call that follows.
    [[nodiscard]] Error already_lost(std::string_view what) const;
};

struct Frame::Impl {
    Device::Impl* device = nullptr;
    SDL_GPUCommandBuffer* commands = nullptr;
    SDL_GPUTexture* swapchain = nullptr;

    /// Where this frame draws when a capture was asked for.
    ///
    /// Null on an ordinary frame, in which case drawing goes straight to the swapchain. On a
    /// capture frame it is an offscreen colour texture, because the swapchain image cannot be
    /// read: Metal creates it framebuffer-only, so copying from it or sampling it is invalid.
    /// Drawing offscreen and blitting the result to the swapchain gives a frame that is both
    /// visible and readable.
    SDL_GPUTexture* capture_target = nullptr;

    Extent2D extent;
    bool submitted = false;
    bool pass_open = false;
};

struct RenderPass::Impl {
    Frame::Impl* frame = nullptr;
    SDL_GPURenderPass* pass = nullptr;
    /// The target's size and format, so a caller setting a viewport or matching a pipeline
    /// need not assume the window's.
    Extent2D target_extent;
    TextureFormat target_format = TextureFormat::Unknown;
    bool ended = false;
};

namespace detail {

/// Build an error from `what` plus whatever SDL last reported, clearing SDL's slot.
[[nodiscard]] Error gpu_error(ErrorCode code, std::string_view what);

/// Record a download of `region` from `source` into `transfer`. Does not submit.
///
/// The caller chooses whether to wait on a fence or poll one, which is the only difference
/// between a capture and a readback.
void record_texture_download(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* source, Rect2D region,
                             SDL_GPUTransferBuffer* transfer);

/// Copy a finished download out of its staging buffer into the pending record, and release
/// the fence and the staging buffer.
void collect_readback(const Device::Impl& device, PendingReadback& pending) noexcept;

/// Copy a readable colour texture back to memory and store it on the device.
///
/// Submits `commands` and waits for it, because the pixels cannot be read before the copy
/// has finished. The caller must therefore not submit the command buffer itself.
[[nodiscard]] Status capture_texture(Device::Impl& device, SDL_GPUCommandBuffer* commands,
                                     SDL_GPUTexture* source, Extent2D extent);

}  // namespace detail
}  // namespace atlas::rhi
