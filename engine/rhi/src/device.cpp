// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/core/handle.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/platform/internal/sdl_access.hpp>
#include <atlas/platform/window.hpp>
#include <atlas/rhi/device.hpp>

#include "device_impl.hpp"
#include "sdl_gpu_conv.hpp"
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace atlas::rhi {

namespace detail {

Error gpu_error(ErrorCode code, std::string_view what) {
    const char* message = SDL_GetError();
    const std::string_view detail =
        (message != nullptr) ? std::string_view{message} : std::string_view{};
    // Leaving the message in place would let an unrelated later failure inherit it.
    SDL_ClearError();

    if (detail.empty()) {
        return {code, std::string{what}};
    }
    return {code, std::format("{}: {}", what, detail)};
}

}  // namespace detail

namespace {

constexpr log::Category kRhi{"rhi"};

/// Frames the device lets the processor run ahead of the graphics processor.
///
/// Two is the usual compromise: one would serialise the two, and more adds latency between
/// an input and the frame that reflects it for throughput nobody asked for.
constexpr std::uint32_t kFramesInFlight = 2;

}  // namespace

// ---------------------------------------------------------------------------------------
// RenderPass
// ---------------------------------------------------------------------------------------

RenderPass::RenderPass(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

RenderPass::~RenderPass() {
    end();
}

RenderPass::RenderPass(RenderPass&& other) noexcept : m_impl(std::move(other.m_impl)) {}

RenderPass& RenderPass::operator=(RenderPass&& other) noexcept {
    if (this != &other) {
        end();
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

void RenderPass::end() {
    if (m_impl == nullptr || m_impl->ended) {
        return;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    SDL_EndGPURenderPass(m_impl->pass);
    m_impl->ended = true;
    if (m_impl->frame != nullptr) {
        m_impl->frame->pass_open = false;
    }
}

void RenderPass::bind_pipeline(GraphicsPipelineHandle pipeline) {
    if (m_impl == nullptr || m_impl->ended) {
        return;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    const auto* resource = m_impl->frame->device->pipelines.get(pipeline);
    if (resource == nullptr) {
        // A stale handle is a programming error, but not one worth ending the process over
        // in release: skipping the draw is visible and recoverable.
        ATLAS_ASSERT_MSG(false, "bind_pipeline called with a stale or null pipeline handle");
        ATLAS_LOG_ERROR(kRhi, "bind_pipeline: handle does not resolve; the draw is skipped");
        return;
    }
    SDL_BindGPUGraphicsPipeline(m_impl->pass, resource->pipeline);
}

void RenderPass::bind_vertex_buffer(BufferHandle buffer, std::uint64_t offset) {
    if (m_impl == nullptr || m_impl->ended) {
        return;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    const auto* resource = m_impl->frame->device->buffers.get(buffer);
    if (resource == nullptr) {
        ATLAS_ASSERT_MSG(false, "bind_vertex_buffer called with a stale or null handle");
        ATLAS_LOG_ERROR(kRhi, "bind_vertex_buffer: handle does not resolve");
        return;
    }

    SDL_GPUBufferBinding binding{};
    binding.buffer = resource->buffer;
    binding.offset = static_cast<std::uint32_t>(offset);
    SDL_BindGPUVertexBuffers(m_impl->pass, 0, &binding, 1);
}

void RenderPass::draw(std::uint32_t vertex_count, std::uint32_t instance_count,
                      std::uint32_t first_vertex, std::uint32_t first_instance) {
    if (m_impl == nullptr || m_impl->ended) {
        return;
    }
    ATLAS_ASSERT_MAIN_THREAD();
    SDL_DrawGPUPrimitives(m_impl->pass, vertex_count, instance_count, first_vertex, first_instance);
}

void RenderPass::push_debug_group(std::string_view label) {
    if (m_impl == nullptr || m_impl->ended) {
        return;
    }
    const std::string text{label};
    SDL_PushGPUDebugGroup(m_impl->frame->commands, text.c_str());
}

void RenderPass::pop_debug_group() {
    if (m_impl == nullptr || m_impl->ended) {
        return;
    }
    SDL_PopGPUDebugGroup(m_impl->frame->commands);
}

void RenderPass::insert_debug_label(std::string_view label) {
    if (m_impl == nullptr || m_impl->ended) {
        return;
    }
    const std::string text{label};
    SDL_InsertGPUDebugLabel(m_impl->frame->commands, text.c_str());
}

// ---------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------

Frame::Frame(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

namespace {

/// Release an unsubmitted frame's command buffer.
///
/// Which of the two ways is legal depends on whether a swapchain image was acquired. SDL
/// refuses to cancel a command buffer that holds one, and says so through an assertion
/// dialog: the image has been taken from the swapchain and something has to give it back.
/// Submitting does that, presenting whatever was drawn, which for an abandoned frame is
/// usually nothing. Only a frame that never got an image can be cancelled outright.
void discard_frame(Frame::Impl& frame) {
    if (frame.commands == nullptr) {
        return;
    }
    if (frame.swapchain != nullptr) {
        SDL_SubmitGPUCommandBuffer(frame.commands);
    } else {
        SDL_CancelGPUCommandBuffer(frame.commands);
    }
    frame.submitted = true;
    if (frame.device != nullptr) {
        frame.device->frame_open = false;
    }
}

}  // namespace

Frame::~Frame() {
    // Abandoned partway through, which is what should happen when an error aborts a frame.
    if (m_impl != nullptr && !m_impl->submitted) {
        discard_frame(*m_impl);
    }
}

Frame::Frame(Frame&& other) noexcept : m_impl(std::move(other.m_impl)) {}

Frame& Frame::operator=(Frame&& other) noexcept {
    if (this != &other) {
        if (m_impl != nullptr && !m_impl->submitted) {
            discard_frame(*m_impl);
        }
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

bool Frame::has_swapchain_target() const noexcept {
    return m_impl != nullptr && m_impl->swapchain != nullptr;
}

Extent2D Frame::swapchain_extent() const noexcept {
    return m_impl != nullptr ? m_impl->extent : Extent2D{};
}

TextureFormat Frame::swapchain_format() const noexcept {
    if (m_impl == nullptr || m_impl->device == nullptr) {
        return TextureFormat::Unknown;
    }
    return m_impl->device->swapchain_format;
}

Result<RenderPass> Frame::begin_render_pass(const RenderPassDesc& desc) {
    ATLAS_ASSERT_MAIN_THREAD();

    if (m_impl == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "begin_render_pass on an invalid frame"));
    }
    if (m_impl->swapchain == nullptr) {
        return std::unexpected(
            Error(ErrorCode::Unavailable,
                  "this frame has no swapchain target, so there is nothing to draw into. The "
                  "window is probably minimised; check has_swapchain_target() first."));
    }
    if (m_impl->pass_open) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "a render pass is already open on this frame"));
    }

    SDL_GPUColorTargetInfo target{};
    target.texture = m_impl->swapchain;
    target.load_op = detail::to_sdl(desc.colour.load);
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.clear_color = SDL_FColor{
        .r = desc.colour.clear_colour.r,
        .g = desc.colour.clear_colour.g,
        .b = desc.colour.clear_colour.b,
        .a = desc.colour.clear_colour.a,
    };

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(m_impl->commands, &target, 1, nullptr);
    if (pass == nullptr) {
        return std::unexpected(
            detail::gpu_error(ErrorCode::Internal, "beginning a render pass failed"));
    }

    if (!desc.debug_name.empty()) {
        const std::string label{desc.debug_name};
        SDL_InsertGPUDebugLabel(m_impl->commands, label.c_str());
    }

    m_impl->pass_open = true;

    auto impl = std::make_unique<RenderPass::Impl>();
    impl->frame = m_impl.get();
    impl->pass = pass;
    return RenderPass{std::move(impl)};
}

// ---------------------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------------------

Device::Device(std::unique_ptr<Impl> impl) noexcept : m_impl(std::move(impl)) {}

Result<Device> Device::create(const DeviceDesc& desc, const platform::Window& window) {
    ATLAS_ASSERT_MAIN_THREAD();

    if (!window.valid()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     "cannot create a graphics device for an invalid window"));
    }

    SDL_Window* native = platform::internal::native_handle(window);
    if (native == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "the window has no native handle"));
    }

    // Ask for every format Atlas can supply. SDL picks a backend that accepts one of them,
    // and the caller then asks which formats the device actually took.
    const SDL_GPUShaderFormat requested =
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_DXIL;

    const char* preferred = nullptr;
    switch (desc.preferred_backend) {
    case Backend::Metal: preferred = "metal"; break;
    case Backend::Vulkan: preferred = "vulkan"; break;
    case Backend::Direct3D12: preferred = "direct3d12"; break;
    case Backend::Unknown: break;
    }

    SDL_GPUDevice* device = SDL_CreateGPUDevice(requested, desc.debug, preferred);
    if (device == nullptr) {
        return std::unexpected(detail::gpu_error(
            ErrorCode::GpuDeviceCreationFailed,
            "no graphics backend could be created. This is expected on a machine with no "
            "display or no supported graphics driver"));
    }

    if (!SDL_ClaimWindowForGPUDevice(device, native)) {
        auto error = detail::gpu_error(ErrorCode::GpuDeviceCreationFailed,
                                       "the device could not present to this window");
        SDL_DestroyGPUDevice(device);
        return std::unexpected(std::move(error));
    }

    SDL_SetGPUAllowedFramesInFlight(device, kFramesInFlight);

    auto impl = std::make_unique<Impl>();
    impl->device = device;
    impl->window = native;
    impl->shader_formats = SDL_GetGPUShaderFormats(device);

    const char* driver = SDL_GetGPUDeviceDriver(device);
    impl->backend_name = (driver != nullptr) ? driver : "unknown";
    impl->backend = detail::backend_from_driver(driver);
    impl->swapchain_format = detail::from_sdl(SDL_GetGPUSwapchainTextureFormat(device, native));

    ATLAS_LOG_INFO(kRhi, "graphics device ready: backend={} validation={} swapchain={}",
                   impl->backend_name, desc.debug, to_string(impl->swapchain_format));
    ATLAS_LOG_INFO(kRhi, "shader formats accepted: spirv={} msl={} dxil={}",
                   (impl->shader_formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0,
                   (impl->shader_formats & SDL_GPU_SHADERFORMAT_MSL) != 0,
                   (impl->shader_formats & SDL_GPU_SHADERFORMAT_DXIL) != 0);

    return Device{std::move(impl)};
}

Device::~Device() {
    if (m_impl == nullptr || m_impl->device == nullptr) {
        return;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    // Nothing may be destroyed while the graphics processor might still be reading it.
    SDL_WaitForGPUIdle(m_impl->device);

    // Report anything still live before tearing it down. A resource the application forgot
    // to destroy is a leak in the application, and naming it is more useful than silently
    // cleaning up and letting the same mistake grow.
    //
    // Written with fprintf rather than through the logger: formatting a log message
    // allocates, and an allocation failure here would throw out of a destructor and end the
    // process during unwinding. stderr takes a pointer and some integers and cannot fail
    // that way.
    const auto counts = resource_counts();
    if (counts.total() > 0) {
        std::fprintf(stderr,
                     "atlas rhi: %zu graphics resources were still live at device shutdown "
                     "(buffers=%zu shaders=%zu pipelines=%zu)\n",
                     counts.total(), counts.buffers, counts.shaders, counts.pipelines);

        m_impl->buffers.for_each_live([](BufferHandle, const BufferResource& resource) {
            std::fprintf(stderr, "  leaked buffer '%s'\n", resource.debug_name.c_str());
        });
        m_impl->shaders.for_each_live([](ShaderHandle, const ShaderResource& resource) {
            std::fprintf(stderr, "  leaked shader '%s'\n", resource.debug_name.c_str());
        });
        m_impl->pipelines.for_each_live(
            [](GraphicsPipelineHandle, const PipelineResource& resource) {
                std::fprintf(stderr, "  leaked pipeline '%s'\n", resource.debug_name.c_str());
            });
    }

    m_impl->pipelines.for_each_live([this](GraphicsPipelineHandle, PipelineResource& resource) {
        SDL_ReleaseGPUGraphicsPipeline(m_impl->device, resource.pipeline);
    });
    m_impl->shaders.for_each_live([this](ShaderHandle, ShaderResource& resource) {
        SDL_ReleaseGPUShader(m_impl->device, resource.shader);
    });
    m_impl->buffers.for_each_live([this](BufferHandle, BufferResource& resource) {
        SDL_ReleaseGPUBuffer(m_impl->device, resource.buffer);
    });

    SDL_ReleaseWindowFromGPUDevice(m_impl->device, m_impl->window);
    SDL_DestroyGPUDevice(m_impl->device);
    m_impl->device = nullptr;
}

Device::Device(Device&& other) noexcept : m_impl(std::move(other.m_impl)) {}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

Backend Device::backend() const noexcept {
    return m_impl != nullptr ? m_impl->backend : Backend::Unknown;
}

std::string_view Device::backend_name() const noexcept {
    return m_impl != nullptr ? std::string_view{m_impl->backend_name} : std::string_view{};
}

bool Device::supports_shader_format(ShaderFormat format) const noexcept {
    if (m_impl == nullptr) {
        return false;
    }
    return (m_impl->shader_formats & detail::to_sdl(format)) != 0;
}

TextureFormat Device::swapchain_format() const noexcept {
    return m_impl != nullptr ? m_impl->swapchain_format : TextureFormat::Unknown;
}

Device::ResourceCounts Device::resource_counts() const noexcept {
    if (m_impl == nullptr) {
        return {};
    }
    return ResourceCounts{
        .buffers = m_impl->buffers.size(),
        .shaders = m_impl->shaders.size(),
        .pipelines = m_impl->pipelines.size(),
    };
}

Result<Frame> Device::begin_frame() {
    ATLAS_ZONE_NAMED("Device::begin_frame");
    ATLAS_ASSERT_MAIN_THREAD();

    if (m_impl == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "begin_frame on an invalid device"));
    }
    if (m_impl->frame_open) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  "a frame is already open; submit or drop it before beginning another"));
    }

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(m_impl->device);
    if (commands == nullptr) {
        return std::unexpected(
            detail::gpu_error(ErrorCode::Internal, "acquiring a command buffer failed"));
    }

    auto impl = std::make_unique<Frame::Impl>();
    impl->device = m_impl.get();
    impl->commands = commands;

    // Waiting here rather than acquiring without a wait: the wait is what limits the
    // processor to the frames-in-flight budget, and without it the queue grows until
    // latency is measured in whole frames.
    SDL_GPUTexture* swapchain = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, m_impl->window, &swapchain, &width,
                                               &height)) {
        // Not an error: a minimised window has no image to draw into, and the frame is
        // still valid for work that does not touch the swapchain.
        ATLAS_LOG_DEBUG(kRhi, "no swapchain image this frame: {}", SDL_GetError());
        SDL_ClearError();
    }

    impl->swapchain = swapchain;
    impl->extent = Extent2D{.width = width, .height = height};

    m_impl->frame_open = true;
    return Frame{std::move(impl)};
}

Status Device::end_frame(Frame&& frame) {
    ATLAS_ZONE_NAMED("Device::end_frame");
    ATLAS_ASSERT_MAIN_THREAD();

    if (m_impl == nullptr) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "end_frame on an invalid device"));
    }
    if (!frame.valid()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "end_frame on an invalid frame"));
    }

    Frame local = std::move(frame);
    ATLAS_ASSERT_MSG(!local.m_impl->pass_open, "a render pass was still open at end_frame");

    // A capture submits the command buffer itself, because it has to wait on a fence before
    // the pixels can be read.
    if (m_impl->capture_requested && local.m_impl->swapchain != nullptr) {
        local.m_impl->submitted = true;
        m_impl->frame_open = false;
        return detail::capture_swapchain(*m_impl, local.m_impl->commands, local.m_impl->swapchain,
                                         local.m_impl->extent);
    }

    const bool submitted = SDL_SubmitGPUCommandBuffer(local.m_impl->commands);
    local.m_impl->submitted = true;
    m_impl->frame_open = false;

    if (!submitted) {
        return std::unexpected(
            detail::gpu_error(ErrorCode::Internal, "submitting the frame failed"));
    }
    return ok();
}

Status Device::wait_idle() {
    ATLAS_ASSERT_MAIN_THREAD();
    if (m_impl == nullptr) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "wait_idle on an invalid device"));
    }
    if (!SDL_WaitForGPUIdle(m_impl->device)) {
        return std::unexpected(
            detail::gpu_error(ErrorCode::Internal, "waiting for the device failed"));
    }
    return ok();
}

}  // namespace atlas::rhi
