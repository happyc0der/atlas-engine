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

using detail::gpu_error;

}  // namespace

Result<BufferHandle> Device::create_buffer(const BufferDesc& desc) {
    ATLAS_ASSERT_MAIN_THREAD();

    if (m_impl == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "create_buffer on an invalid device"));
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
        return std::unexpected(gpu_error(
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
            gpu_error(ErrorCode::ResourceCreationFailed, "creating a transfer buffer failed"));
    }

    void* mapped = SDL_MapGPUTransferBuffer(m_impl->device, transfer, false);
    if (mapped == nullptr) {
        auto error = gpu_error(ErrorCode::Internal, "mapping the transfer buffer failed");
        SDL_ReleaseGPUTransferBuffer(m_impl->device, transfer);
        return std::unexpected(std::move(error));
    }
    std::memcpy(mapped, data.data(), data.size());
    SDL_UnmapGPUTransferBuffer(m_impl->device, transfer);

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(m_impl->device);
    if (commands == nullptr) {
        auto error = gpu_error(ErrorCode::Internal, "acquiring a command buffer for upload failed");
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
        auto error = gpu_error(ErrorCode::Internal, "submitting the upload failed");
        SDL_ReleaseGPUTransferBuffer(m_impl->device, transfer);
        return std::unexpected(std::move(error));
    }

    const bool waited = SDL_WaitForGPUFences(m_impl->device, true, &fence, 1);
    SDL_ReleaseGPUFence(m_impl->device, fence);
    SDL_ReleaseGPUTransferBuffer(m_impl->device, transfer);

    if (!waited) {
        return std::unexpected(gpu_error(ErrorCode::Internal, "waiting for the upload failed"));
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
    // No resources yet: the first triangle reads nothing. Counts arrive with the first
    // uniform buffer and texture, in M3, and must match the shader exactly.
    info.num_samplers = 0;
    info.num_storage_textures = 0;
    info.num_storage_buffers = 0;
    info.num_uniform_buffers = 0;

    SDL_GPUShader* shader = SDL_CreateGPUShader(m_impl->device, &info);
    if (shader == nullptr) {
        return std::unexpected(gpu_error(
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

    std::vector<SDL_GPUVertexAttribute> attributes;
    attributes.reserve(desc.vertex_layout.attributes.size());
    for (const auto& attribute : desc.vertex_layout.attributes) {
        attributes.push_back(SDL_GPUVertexAttribute{
            .location = attribute.location,
            .buffer_slot = 0,
            .format = detail::to_sdl(attribute.format),
            .offset = attribute.offset,
        });
    }

    SDL_GPUVertexBufferDescription buffer_description{};
    buffer_description.slot = 0;
    buffer_description.pitch = desc.vertex_layout.stride;
    buffer_description.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    buffer_description.instance_step_rate = 0;

    SDL_GPUColorTargetDescription colour_target{};
    colour_target.format = detail::to_sdl(colour_format);

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex->shader;
    info.fragment_shader = fragment->shader;
    info.primitive_type = detail::to_sdl(desc.topology);
    info.target_info.num_color_targets = 1;
    info.target_info.color_target_descriptions = &colour_target;

    // A stride of zero means the pipeline reads no vertex buffer, which is how the first
    // triangle is drawn: entirely from the vertex index.
    if (desc.vertex_layout.stride > 0 && !attributes.empty()) {
        info.vertex_input_state.num_vertex_buffers = 1;
        info.vertex_input_state.vertex_buffer_descriptions = &buffer_description;
        info.vertex_input_state.num_vertex_attributes =
            static_cast<std::uint32_t>(attributes.size());
        info.vertex_input_state.vertex_attributes = attributes.data();
    }

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(m_impl->device, &info);
    if (pipeline == nullptr) {
        return std::unexpected(
            gpu_error(ErrorCode::PipelineCreationFailed,
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

namespace detail {

Status capture_swapchain(Device::Impl& device, SDL_GPUCommandBuffer* commands,
                         SDL_GPUTexture* swapchain, Extent2D extent) {
    ATLAS_ZONE_NAMED("capture swapchain");

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
            gpu_error(ErrorCode::ResourceCreationFailed, "creating a download buffer failed"));
    }

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);

    SDL_GPUTextureRegion region{};
    region.texture = swapchain;
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
        auto error = gpu_error(ErrorCode::Internal, "submitting the capture failed");
        SDL_ReleaseGPUTransferBuffer(device.device, transfer);
        return std::unexpected(std::move(error));
    }

    const bool waited = SDL_WaitForGPUFences(device.device, true, &fence, 1);
    SDL_ReleaseGPUFence(device.device, fence);

    if (!waited) {
        auto error = gpu_error(ErrorCode::Internal, "waiting for the capture failed");
        SDL_ReleaseGPUTransferBuffer(device.device, transfer);
        return std::unexpected(std::move(error));
    }

    const void* mapped = SDL_MapGPUTransferBuffer(device.device, transfer, false);
    if (mapped == nullptr) {
        auto error = gpu_error(ErrorCode::Internal, "mapping the captured pixels failed");
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
