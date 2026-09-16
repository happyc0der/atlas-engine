// SPDX-License-Identifier: GPL-3.0-or-later
//
// Textures and samplers. Split from resources.cpp so that neither file has to be read whole
// to follow the other.
#include <atlas/core/assert.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/rhi/device.hpp>

#include "device_impl.hpp"
#include "sdl_gpu_conv.hpp"
#include <SDL3/SDL_gpu.h>

#include <cstring>
#include <format>
#include <string>
#include <utility>

namespace atlas::rhi {
namespace {}  // namespace

Result<TextureHandle> Device::create_texture(const TextureDesc& desc) {
    ATLAS_ASSERT_MAIN_THREAD();

    if (m_impl == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "create_texture on an invalid device"));
    }
    if (m_impl->health.lost()) {
        return std::unexpected(m_impl->already_lost("create a texture"));
    }
    if (desc.width == 0 || desc.height == 0) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("texture '{}' must have a non-zero size, got {}x{}", desc.debug_name,
                              desc.width, desc.height)));
    }
    if (desc.format == TextureFormat::Unknown) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     std::format("texture '{}' has no format", desc.debug_name)));
    }
    if (!desc.usage.sampled && !desc.usage.colour_target) {
        // A texture nothing may do anything with is a mistake at the call site, not a
        // resource worth allocating.
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("texture '{}' declares no usage, so nothing could read or write it",
                              desc.debug_name)));
    }

    const SDL_GPUTextureUsageFlags usage = detail::to_sdl(desc.usage);

    // Asked before creating, because SDL returns a null texture with a generic message for
    // an unsupported combination, and the useful part is which format and which usage.
    if (!SDL_GPUTextureSupportsFormat(m_impl->device, detail::to_sdl(desc.format),
                                      SDL_GPU_TEXTURETYPE_2D, usage)) {
        SDL_ClearError();
        return std::unexpected(Error(
            ErrorCode::NotSupported,
            std::format("this backend ({}) cannot use format {} as {}, wanted for texture '{}'",
                        m_impl->backend_name, to_string(desc.format), to_string(desc.usage),
                        desc.debug_name)));
    }

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = detail::to_sdl(desc.format);
    info.usage = usage;
    info.width = desc.width;
    info.height = desc.height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(m_impl->device, &info);
    if (texture == nullptr) {
        return std::unexpected(
            m_impl->fail(ErrorCode::ResourceCreationFailed,
                         std::format("creating texture '{}' ({}x{}, {}) failed", desc.debug_name,
                                     desc.width, desc.height, to_string(desc.format))));
    }

    const std::string name{desc.debug_name};
    if (!name.empty()) {
        SDL_SetGPUTextureName(m_impl->device, texture, name.c_str());
    }

    auto handle = m_impl->textures.insert(TextureResource{
        .texture = texture,
        .width = desc.width,
        .height = desc.height,
        .format = desc.format,
        .usage = desc.usage,
        .debug_name = name,
    });
    if (!handle) {
        SDL_ReleaseGPUTexture(m_impl->device, texture);
        return std::unexpected(std::move(handle).error().context("registering a texture"));
    }
    return *handle;
}

Status Device::upload_texture(TextureHandle texture, std::span<const std::byte> pixels) {
    ATLAS_ZONE_NAMED("Device::upload_texture");
    ATLAS_ASSERT_MAIN_THREAD();

    if (m_impl == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "upload_texture on an invalid device"));
    }
    if (m_impl->health.lost()) {
        return std::unexpected(m_impl->already_lost("upload to a texture"));
    }

    const auto* resource = m_impl->textures.get(texture);
    if (resource == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "upload_texture: the handle is null or stale"));
    }

    const std::uint32_t pixel_size = byte_size(resource->format);
    const std::size_t expected =
        static_cast<std::size_t>(resource->width) * resource->height * pixel_size;
    if (pixels.size() != expected) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("texture '{}' is {}x{} in {}, which needs exactly {} bytes; got {}",
                              resource->debug_name, resource->width, resource->height,
                              to_string(resource->format), expected, pixels.size())));
    }

    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = static_cast<std::uint32_t>(pixels.size());

    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(m_impl->device, &transfer_info);
    if (transfer == nullptr) {
        return std::unexpected(
            m_impl->fail(ErrorCode::ResourceCreationFailed, "creating a transfer buffer failed"));
    }

    void* mapped = SDL_MapGPUTransferBuffer(m_impl->device, transfer, false);
    if (mapped == nullptr) {
        auto error = m_impl->fail(ErrorCode::Internal, "mapping the transfer buffer failed");
        SDL_ReleaseGPUTransferBuffer(m_impl->device, transfer);
        return std::unexpected(std::move(error));
    }
    std::memcpy(mapped, pixels.data(), pixels.size());
    SDL_UnmapGPUTransferBuffer(m_impl->device, transfer);

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(m_impl->device);
    if (commands == nullptr) {
        auto error = m_impl->fail(ErrorCode::Internal, "acquiring a command buffer failed");
        SDL_ReleaseGPUTransferBuffer(m_impl->device, transfer);
        return std::unexpected(std::move(error));
    }

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);

    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = transfer;
    source.offset = 0;
    source.pixels_per_row = resource->width;
    source.rows_per_layer = resource->height;

    SDL_GPUTextureRegion destination{};
    destination.texture = resource->texture;
    destination.w = resource->width;
    destination.h = resource->height;
    destination.d = 1;

    SDL_UploadToGPUTexture(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (fence == nullptr) {
        auto error = m_impl->fail(ErrorCode::Internal, "submitting the texture upload failed");
        SDL_ReleaseGPUTransferBuffer(m_impl->device, transfer);
        return std::unexpected(std::move(error));
    }

    const bool waited = SDL_WaitForGPUFences(m_impl->device, true, &fence, 1);
    SDL_ReleaseGPUFence(m_impl->device, fence);
    SDL_ReleaseGPUTransferBuffer(m_impl->device, transfer);

    if (!waited) {
        return std::unexpected(m_impl->fail(ErrorCode::Internal, "waiting for the upload failed"));
    }
    return ok();
}

void Device::destroy_texture(TextureHandle texture) {
    if (m_impl == nullptr) {
        return;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    const auto* resource = m_impl->textures.get(texture);
    if (resource == nullptr) {
        return;
    }
    SDL_ReleaseGPUTexture(m_impl->device, resource->texture);
    m_impl->textures.destroy(texture);
}

Result<SamplerHandle> Device::create_sampler(const SamplerDesc& desc) {
    ATLAS_ASSERT_MAIN_THREAD();

    if (m_impl == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "create_sampler on an invalid device"));
    }
    if (m_impl->health.lost()) {
        return std::unexpected(m_impl->already_lost("create a sampler"));
    }

    SDL_GPUSamplerCreateInfo info{};
    info.min_filter = detail::to_sdl(desc.min_filter);
    info.mag_filter = detail::to_sdl(desc.mag_filter);
    info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    info.address_mode_u = detail::to_sdl(desc.address_u);
    info.address_mode_v = detail::to_sdl(desc.address_v);
    info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(m_impl->device, &info);
    if (sampler == nullptr) {
        return std::unexpected(
            m_impl->fail(ErrorCode::ResourceCreationFailed,
                         std::format("creating sampler '{}' failed", desc.debug_name)));
    }

    auto handle = m_impl->samplers.insert(SamplerResource{
        .sampler = sampler,
        .debug_name = std::string{desc.debug_name},
    });
    if (!handle) {
        SDL_ReleaseGPUSampler(m_impl->device, sampler);
        return std::unexpected(std::move(handle).error().context("registering a sampler"));
    }
    return *handle;
}

void Device::destroy_sampler(SamplerHandle sampler) {
    if (m_impl == nullptr) {
        return;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    const auto* resource = m_impl->samplers.get(sampler);
    if (resource == nullptr) {
        return;
    }
    SDL_ReleaseGPUSampler(m_impl->device, resource->sampler);
    m_impl->samplers.destroy(sampler);
}

}  // namespace atlas::rhi
