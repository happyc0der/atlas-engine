// SPDX-License-Identifier: GPL-3.0-or-later
//
// Resource creation, upload, and readback. Split from device.cpp so that neither file has
// to be read whole to follow the other.
#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/rhi/device.hpp>

#include "device_impl.hpp"
#include "sdl_gpu_conv.hpp"
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>

#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace atlas::rhi {
namespace {

constexpr log::Category kRhi{"rhi"};

}  // namespace

Result<BufferHandle> Device::create_buffer(const BufferDesc& desc) {
    ATLAS_ASSERT_MAIN_THREAD();

    if (m_impl == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "create_buffer on an invalid device"));
    }
    if (m_impl->health.lost()) {
        return std::unexpected(m_impl->already_lost("create a buffer"));
    }
    if (desc.size == 0) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "a buffer must have a non-zero size"));
    }

    SDL_GPUBufferCreateInfo info{};
    info.usage = detail::to_sdl(desc.usage);
    info.size = static_cast<std::uint32_t>(desc.size);

    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(m_impl->device, &info);
    if (buffer == nullptr) {
        return std::unexpected(m_impl->fail(
            ErrorCode::ResourceCreationFailed,
            std::format("creating buffer '{}' of {} bytes failed", desc.debug_name, desc.size)));
    }

    const std::string name{desc.debug_name};
    if (!name.empty()) {
        SDL_SetGPUBufferName(m_impl->device, buffer, name.c_str());
    }

    auto handle = m_impl->buffers.insert(BufferResource{
        .buffer = buffer,
        .size = desc.size,
        .debug_name = name,
    });
    if (!handle) {
        SDL_ReleaseGPUBuffer(m_impl->device, buffer);
        return std::unexpected(std::move(handle).error().context("registering a buffer"));
    }
    return *handle;
}

Status Device::upload_buffer(BufferHandle buffer, std::span<const std::byte> data,
                             std::uint64_t offset) {
    ATLAS_ZONE_NAMED("Device::upload_buffer");
    ATLAS_ASSERT_MAIN_THREAD();

    if (m_impl == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "upload_buffer on an invalid device"));
    }
    if (m_impl->health.lost()) {
        return std::unexpected(m_impl->already_lost("upload to a buffer"));
    }

    const auto* resource = m_impl->buffers.get(buffer);
    if (resource == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "upload_buffer: the handle is null or stale"));
    }
    if (data.empty()) {
        return ok();
    }
    if (offset + data.size() > resource->size) {
        return std::unexpected(Error(
            ErrorCode::OutOfRange,
            std::format("upload of {} bytes at offset {} does not fit buffer '{}' of {} bytes",
                        data.size(), offset, resource->debug_name, resource->size)));
    }

    // Data reaches the graphics processor through a transfer buffer: write it into memory
    // both sides can see, then copy it across.
    SDL_GPUTransferBufferCreateInfo transfer_info{};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = static_cast<std::uint32_t>(data.size());

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
    std::memcpy(mapped, data.data(), data.size());
    SDL_UnmapGPUTransferBuffer(m_impl->device, transfer);

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(m_impl->device);
    if (commands == nullptr) {
        auto error =
            m_impl->fail(ErrorCode::Internal, "acquiring a command buffer for upload failed");
        SDL_ReleaseGPUTransferBuffer(m_impl->device, transfer);
        return std::unexpected(std::move(error));
    }

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = transfer;
    source.offset = 0;

    SDL_GPUBufferRegion destination{};
    destination.buffer = resource->buffer;
    destination.offset = static_cast<std::uint32_t>(offset);
    destination.size = static_cast<std::uint32_t>(data.size());

    SDL_UploadToGPUBuffer(copy, &source, &destination, false);
    SDL_EndGPUCopyPass(copy);

    // Waiting makes this an initialisation-time operation. A streaming path that does not
    // stall belongs with the first thing that updates a buffer every frame, and does not
    // exist yet.
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (fence == nullptr) {
        auto error = m_impl->fail(ErrorCode::Internal, "submitting the upload failed");
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

void Device::destroy_buffer(BufferHandle buffer) {
    if (m_impl == nullptr) {
        return;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    const auto* resource = m_impl->buffers.get(buffer);
    if (resource == nullptr) {
        return;  // Already destroyed, or never valid. Both are harmless.
    }

    // SDL defers the release until the graphics processor has finished with the resource,
    // so there is no deletion queue here. ADR-0002 records that reliance; a backend without
    // it would need one.
    SDL_ReleaseGPUBuffer(m_impl->device, resource->buffer);
    m_impl->buffers.destroy(buffer);
}

Result<ShaderHandle> Device::create_shader(const ShaderDesc& desc) {
    ATLAS_ASSERT_MAIN_THREAD();

    if (m_impl == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "create_shader on an invalid device"));
    }
    if (m_impl->health.lost()) {
        return std::unexpected(m_impl->already_lost("create a shader"));
    }
    if (desc.code.empty()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     std::format("shader '{}' has no code", desc.debug_name)));
    }
    if (!supports_shader_format(desc.format)) {
        return std::unexpected(
            Error(ErrorCode::NotSupported,
                  std::format("shader '{}' is {}, which the {} backend does not accept",
                              desc.debug_name, to_string(desc.format), m_impl->backend_name)));
    }

    const std::string entry{desc.entry_point};

    SDL_GPUShaderCreateInfo info{};
    // SDL wants unsigned bytes and Atlas carries std::byte, which is the same object
    // representation. Two analyser checks disagree about how to say so: one bans
    // reinterpret_cast outright, the other bans routing around it through void*. Byte-wise
    // reinterpretation is exactly what reinterpret_cast is for, so that is what this uses.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    info.code = reinterpret_cast<const Uint8*>(desc.code.data());
    info.code_size = desc.code.size();
    info.entrypoint = entry.c_str();
    info.format = detail::to_sdl(desc.format);
    info.stage = detail::to_sdl(desc.stage);
    // These must match the compiled shader exactly. They are read out of it by reflection
    // in tools/cook_shaders.py and reach here as generated constants, so a shader that gains
    // a resource cannot leave a stale count behind.
    info.num_samplers = desc.samplers;
    info.num_storage_textures = desc.storage_textures;
    info.num_storage_buffers = desc.storage_buffers;
    info.num_uniform_buffers = desc.uniform_buffers;

    SDL_GPUShader* shader = SDL_CreateGPUShader(m_impl->device, &info);
    if (shader == nullptr) {
        return std::unexpected(m_impl->fail(
            ErrorCode::ShaderCompilationFailed,
            std::format("creating {} shader '{}' ({}, entry point '{}') failed",
                        to_string(desc.stage), desc.debug_name, to_string(desc.format), entry)));
    }

    auto handle = m_impl->shaders.insert(ShaderResource{
        .shader = shader,
        .debug_name = std::string{desc.debug_name},
    });
    if (!handle) {
        SDL_ReleaseGPUShader(m_impl->device, shader);
        return std::unexpected(std::move(handle).error().context("registering a shader"));
    }
    return *handle;
}

void Device::destroy_shader(ShaderHandle shader) {
    if (m_impl == nullptr) {
        return;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    const auto* resource = m_impl->shaders.get(shader);
    if (resource == nullptr) {
        return;
    }
    SDL_ReleaseGPUShader(m_impl->device, resource->shader);
    m_impl->shaders.destroy(shader);
}

Result<GraphicsPipelineHandle> Device::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
    ATLAS_ASSERT_MAIN_THREAD();

    if (m_impl == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "create_graphics_pipeline on an invalid device"));
    }
    if (m_impl->health.lost()) {
        return std::unexpected(m_impl->already_lost("create a graphics pipeline"));
    }

    const auto* vertex = m_impl->shaders.get(desc.vertex_shader);
    const auto* fragment = m_impl->shaders.get(desc.fragment_shader);
    if (vertex == nullptr || fragment == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("pipeline '{}' needs a live vertex and fragment shader; one handle "
                              "is null or stale",
                              desc.debug_name)));
    }

    const TextureFormat colour_format = desc.colour_format == TextureFormat::Unknown
                                            ? m_impl->swapchain_format
                                            : desc.colour_format;
    if (colour_format == TextureFormat::Unknown) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("pipeline '{}' has no colour target format", desc.debug_name)));
    }

    // Refused here rather than left to the backend, because the backends disagree and both
    // answers are bad. Metal aborts the process on a validation assertion; Vulkan accepts it
    // and blends the identifiers, producing values nothing ever wrote. Measured on both.
    if (is_integer_format(colour_format) && desc.blend != BlendMode::Replace) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("pipeline '{}' draws into {}, an integer format, with blend mode {}. "
                              "An integer target cannot be blended; use BlendMode::Replace.",
                              desc.debug_name, to_string(colour_format), to_string(desc.blend))));
    }

    // One SDL description per stream, and one attribute entry per attribute across all of
    // them, each tagged with the slot it belongs to. Two streams is the batching case: a
    // shared unit quad in slot zero and per-instance data in slot one.
    std::vector<SDL_GPUVertexBufferDescription> stream_descriptions;
    std::vector<SDL_GPUVertexAttribute> attributes;
    stream_descriptions.reserve(desc.vertex_layout.streams.size());

    for (std::size_t slot = 0; slot < desc.vertex_layout.streams.size(); ++slot) {
        const auto& stream = desc.vertex_layout.streams[slot];
        if (stream.stride == 0) {
            return std::unexpected(
                Error(ErrorCode::InvalidArgument,
                      std::format("pipeline '{}': vertex stream {} has a stride of zero",
                                  desc.debug_name, slot)));
        }

        stream_descriptions.push_back(SDL_GPUVertexBufferDescription{
            .slot = static_cast<std::uint32_t>(slot),
            .pitch = stream.stride,
            .input_rate = stream.per_instance ? SDL_GPU_VERTEXINPUTRATE_INSTANCE
                                              : SDL_GPU_VERTEXINPUTRATE_VERTEX,
            .instance_step_rate = 0,
        });

        for (const auto& attribute : stream.attributes) {
            // An attribute that reads past the end of its element would sample whatever
            // followed it in memory, which shows up as geometry that is wrong in a way that
            // looks like a shader bug.
            if (attribute.offset + byte_size(attribute.format) > stream.stride) {
                return std::unexpected(Error(
                    ErrorCode::InvalidArgument,
                    std::format("pipeline '{}': attribute at location {} reads {} bytes at "
                                "offset {}, past the end of a {}-byte vertex",
                                desc.debug_name, attribute.location, byte_size(attribute.format),
                                attribute.offset, stream.stride)));
            }

            attributes.push_back(SDL_GPUVertexAttribute{
                .location = attribute.location,
                .buffer_slot = static_cast<std::uint32_t>(slot),
                .format = detail::to_sdl(attribute.format),
                .offset = attribute.offset,
            });
        }
    }

    SDL_GPUColorTargetDescription colour_target{};
    colour_target.format = detail::to_sdl(colour_format);

    // The blend mode is part of the descriptor now, which is what the previous comment here
    // said would happen when something needed a different one. An identifier target is that
    // something.
    if (desc.blend == BlendMode::AlphaBlend) {
        // Straight alpha, which is what a sprite with a transparent border needs.
        colour_target.blend_state.enable_blend = true;
        colour_target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        colour_target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colour_target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        colour_target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colour_target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colour_target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    } else {
        colour_target.blend_state.enable_blend = false;
    }

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex->shader;
    info.fragment_shader = fragment->shader;
    info.primitive_type = detail::to_sdl(desc.topology);
    info.target_info.num_color_targets = 1;
    info.target_info.color_target_descriptions = &colour_target;

    // No streams means the pipeline reads no vertex buffer, which is how the first triangle
    // is drawn: entirely from the vertex index.
    if (!stream_descriptions.empty()) {
        info.vertex_input_state.num_vertex_buffers =
            static_cast<std::uint32_t>(stream_descriptions.size());
        info.vertex_input_state.vertex_buffer_descriptions = stream_descriptions.data();
        info.vertex_input_state.num_vertex_attributes =
            static_cast<std::uint32_t>(attributes.size());
        info.vertex_input_state.vertex_attributes = attributes.data();
    }

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(m_impl->device, &info);
    if (pipeline == nullptr) {
        return std::unexpected(
            m_impl->fail(ErrorCode::PipelineCreationFailed,
                         std::format("creating pipeline '{}' failed", desc.debug_name)));
    }

    auto handle = m_impl->pipelines.insert(PipelineResource{
        .pipeline = pipeline,
        .debug_name = std::string{desc.debug_name},
    });
    if (!handle) {
        SDL_ReleaseGPUGraphicsPipeline(m_impl->device, pipeline);
        return std::unexpected(std::move(handle).error().context("registering a pipeline"));
    }
    return *handle;
}

void Device::destroy_graphics_pipeline(GraphicsPipelineHandle pipeline) {
    if (m_impl == nullptr) {
        return;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    const auto* resource = m_impl->pipelines.get(pipeline);
    if (resource == nullptr) {
        return;
    }
    SDL_ReleaseGPUGraphicsPipeline(m_impl->device, resource->pipeline);
    m_impl->pipelines.destroy(pipeline);
}

void Device::request_capture() noexcept {
    if (m_impl != nullptr) {
        m_impl->capture_requested = true;
    }
}

std::optional<Device::Capture> Device::take_capture() noexcept {
    if (m_impl == nullptr) {
        return std::nullopt;
    }
    return std::exchange(m_impl->capture, std::nullopt);
}

Result<ReadbackHandle> Device::request_readback(TextureHandle texture, Rect2D region) {
    ATLAS_ZONE_NAMED("Device::request_readback");
    ATLAS_ASSERT_MAIN_THREAD();

    if (m_impl == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "request_readback on an invalid device"));
    }
    if (m_impl->health.lost()) {
        return std::unexpected(m_impl->already_lost("read a texture back"));
    }

    const TextureResource* resource = m_impl->textures.get(texture);
    if (resource == nullptr) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     "the texture to read back does not resolve; it was "
                                     "destroyed, or the handle is from another device"));
    }

    const std::uint32_t pixel_size = byte_size(resource->format);
    if (pixel_size == 0) {
        return std::unexpected(
            Error(ErrorCode::NotSupported,
                  std::format("texture '{}' has format {}, whose pixel size is not known here",
                              resource->debug_name, to_string(resource->format))));
    }
    if (region.extent.width == 0 || region.extent.height == 0) {
        return std::unexpected(Error(
            ErrorCode::InvalidArgument,
            std::format("an empty region of texture '{}' was asked for", resource->debug_name)));
    }

    // Checked in 64 bits before any of it is used to size an allocation, so a region near
    // the top of the range cannot wrap into a small, plausible one.
    const std::uint64_t right = static_cast<std::uint64_t>(region.x) + region.extent.width;
    const std::uint64_t bottom = static_cast<std::uint64_t>(region.y) + region.extent.height;
    if (right > resource->width || bottom > resource->height) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  std::format("the region ({},{} {}x{}) leaves texture '{}', which is {}x{}",
                              region.x, region.y, region.extent.width, region.extent.height,
                              resource->debug_name, resource->width, resource->height)));
    }

    if (m_impl->readbacks.size() >= kMaxPendingReadbacks) {
        return std::unexpected(
            Error(ErrorCode::Exhausted,
                  std::format("{} readbacks are already outstanding, which is the limit. Something "
                              "is asking and not collecting.",
                              kMaxPendingReadbacks)));
    }

    const std::uint32_t byte_count = region.extent.width * region.extent.height * pixel_size;

    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = byte_count;

    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(m_impl->device, &info);
    if (transfer == nullptr) {
        return std::unexpected(
            m_impl->fail(ErrorCode::ResourceCreationFailed, "creating a readback staging buffer"));
    }

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(m_impl->device);
    if (commands == nullptr) {
        auto error = m_impl->fail(ErrorCode::Internal, "acquiring a command buffer for a readback");
        SDL_ReleaseGPUTransferBuffer(m_impl->device, transfer);
        return std::unexpected(std::move(error));
    }

    detail::record_texture_download(commands, resource->texture, region, transfer);

    // Submitted with a fence and deliberately not waited on. That is the whole difference
    // between this and capture, and the reason picking does not stall a frame.
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (fence == nullptr) {
        auto error = m_impl->fail(ErrorCode::Internal, "submitting a readback");
        SDL_ReleaseGPUTransferBuffer(m_impl->device, transfer);
        return std::unexpected(std::move(error));
    }

    auto ticket = m_impl->readbacks.insert(PendingReadback{
        .fence = fence,
        .transfer = transfer,
        .region = region,
        .format = resource->format,
        .byte_count = byte_count,
        .result = std::nullopt,
        .debug_name = resource->debug_name,
    });
    if (!ticket) {
        SDL_ReleaseGPUFence(m_impl->device, fence);
        SDL_ReleaseGPUTransferBuffer(m_impl->device, transfer);
        return std::unexpected(std::move(ticket).error().context("recording a readback"));
    }
    return *ticket;
}

bool Device::readback_ready(ReadbackHandle ticket) noexcept {
    if (m_impl == nullptr) {
        return false;
    }
    PendingReadback* pending = m_impl->readbacks.get(ticket);
    if (pending == nullptr) {
        return false;
    }
    if (pending->result.has_value()) {
        return true;
    }
    if (pending->fence == nullptr) {
        return false;
    }
    if (!SDL_QueryGPUFence(m_impl->device, pending->fence)) {
        return false;
    }

    // Signalled. Copy the pixels out now and give the staging memory and the fence back,
    // rather than holding them until the caller happens to take the result.
    detail::collect_readback(*m_impl, *pending);
    return pending->result.has_value();
}

Result<Device::Readback> Device::take_readback(ReadbackHandle ticket) {
    ATLAS_ASSERT_MAIN_THREAD();

    if (m_impl == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "take_readback on an invalid device"));
    }
    PendingReadback* pending = m_impl->readbacks.get(ticket);
    if (pending == nullptr) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     "this readback ticket does not resolve; it was already "
                                     "taken, or it is from another device"));
    }
    if (!pending->result.has_value() && !readback_ready(ticket)) {
        return std::unexpected(Error(ErrorCode::Unavailable,
                                     "this readback has not finished yet; ask readback_ready "
                                     "first, or wait_idle to force it"));
    }
    // readback_ready may have reallocated nothing, but re-resolve anyway rather than relying
    // on the pointer surviving a call that can mutate the pool.
    pending = m_impl->readbacks.get(ticket);
    if (pending == nullptr || !pending->result.has_value()) {
        return std::unexpected(
            Error(ErrorCode::Internal, "a readback signalled but produced no pixels"));
    }

    Readback taken = std::move(*pending->result);
    m_impl->readbacks.destroy(ticket);
    return taken;
}

std::size_t Device::pending_readbacks() const noexcept {
    return m_impl != nullptr ? m_impl->readbacks.size() : 0;
}

namespace detail {

void record_texture_download(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* source, Rect2D region,
                             SDL_GPUTransferBuffer* transfer) {
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);

    SDL_GPUTextureRegion source_region{};
    source_region.texture = source;
    source_region.x = region.x;
    source_region.y = region.y;
    source_region.w = region.extent.width;
    source_region.h = region.extent.height;
    source_region.d = 1;

    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = transfer;
    destination.offset = 0;
    destination.pixels_per_row = region.extent.width;
    destination.rows_per_layer = region.extent.height;

    SDL_DownloadFromGPUTexture(copy, &source_region, &destination);
    SDL_EndGPUCopyPass(copy);
}

void collect_readback(Device::Impl& device, PendingReadback& pending) {
    if (pending.fence != nullptr) {
        SDL_ReleaseGPUFence(device.device, pending.fence);
        pending.fence = nullptr;
    }
    if (pending.transfer == nullptr) {
        return;
    }

    void* mapped = SDL_MapGPUTransferBuffer(device.device, pending.transfer, false);
    if (mapped != nullptr) {
        Device::Readback readback;
        readback.region = pending.region;
        readback.format = pending.format;
        readback.pixels.resize(pending.byte_count);
        std::memcpy(readback.pixels.data(), mapped, pending.byte_count);
        SDL_UnmapGPUTransferBuffer(device.device, pending.transfer);
        pending.result = std::move(readback);
    } else {
        ATLAS_LOG_ERROR(kRhi, "mapping the pixels read back from '{}' failed: {}",
                        pending.debug_name, SDL_GetError());
        SDL_ClearError();
    }

    SDL_ReleaseGPUTransferBuffer(device.device, pending.transfer);
    pending.transfer = nullptr;
}

Status capture_texture(Device::Impl& device, SDL_GPUCommandBuffer* commands, SDL_GPUTexture* source,
                       Extent2D extent) {
    ATLAS_ZONE_NAMED("capture texture");

    const std::uint32_t pixel_size = byte_size(device.swapchain_format);
    if (pixel_size == 0 || extent.width == 0 || extent.height == 0) {
        return std::unexpected(Error(ErrorCode::Unavailable,
                                     "cannot capture: the swapchain format or size is unknown"));
    }

    const std::uint32_t byte_count = extent.width * extent.height * pixel_size;

    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = byte_count;

    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device.device, &info);
    if (transfer == nullptr) {
        return std::unexpected(
            device.fail(ErrorCode::ResourceCreationFailed, "creating a download buffer failed"));
    }

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);

    SDL_GPUTextureRegion region{};
    region.texture = source;
    region.w = extent.width;
    region.h = extent.height;
    region.d = 1;

    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = transfer;
    destination.offset = 0;
    destination.pixels_per_row = extent.width;
    destination.rows_per_layer = extent.height;

    SDL_DownloadFromGPUTexture(copy, &region, &destination);
    SDL_EndGPUCopyPass(copy);

    // The command buffer is submitted here rather than by end_frame, because the fence has
    // to be waited on before the transfer buffer can be read.
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (fence == nullptr) {
        auto error = device.fail(ErrorCode::Internal, "submitting the capture failed");
        SDL_ReleaseGPUTransferBuffer(device.device, transfer);
        return std::unexpected(std::move(error));
    }

    const bool waited = SDL_WaitForGPUFences(device.device, true, &fence, 1);
    SDL_ReleaseGPUFence(device.device, fence);

    if (!waited) {
        auto error = device.fail(ErrorCode::Internal, "waiting for the capture failed");
        SDL_ReleaseGPUTransferBuffer(device.device, transfer);
        return std::unexpected(std::move(error));
    }

    const void* mapped = SDL_MapGPUTransferBuffer(device.device, transfer, false);
    if (mapped == nullptr) {
        auto error = device.fail(ErrorCode::Internal, "mapping the captured pixels failed");
        SDL_ReleaseGPUTransferBuffer(device.device, transfer);
        return std::unexpected(std::move(error));
    }

    Device::Capture capture;
    capture.extent = extent;
    capture.format = device.swapchain_format;
    capture.pixels.resize(byte_count);
    std::memcpy(capture.pixels.data(), mapped, byte_count);

    SDL_UnmapGPUTransferBuffer(device.device, transfer);
    SDL_ReleaseGPUTransferBuffer(device.device, transfer);

    device.capture = std::move(capture);
    device.capture_requested = false;

    ATLAS_LOG_INFO(kRhi, "captured {}x{} pixels ({} bytes, {})", extent.width, extent.height,
                   byte_count, to_string(device.swapchain_format));
    return ok();
}

}  // namespace detail

}  // namespace atlas::rhi
