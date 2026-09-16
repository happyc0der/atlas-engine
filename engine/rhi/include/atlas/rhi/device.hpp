// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The graphics device, and the frame and pass scopes that record work for it.
///
/// Thread affinity: every member of every type here is main-thread only, asserted at each
/// entry point. Graphics APIs vary in how much they tolerate otherwise, and a rule that
/// holds everywhere is easier to keep than one that depends on the backend.
///
/// Lifetime: a Frame borrows the device and must not outlive it or the frame it represents;
/// a RenderPass likewise borrows its frame. Both are move-only scopes that close themselves,
/// so a missing end call is not something a caller can forget.

#include <atlas/core/result.hpp>
#include <atlas/rhi/descs.hpp>
#include <atlas/rhi/handles.hpp>
#include <atlas/rhi/types.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;
struct SDL_GPURenderPass;

namespace atlas::platform {
class Window;
}

namespace atlas::rhi {

class Device;
class Frame;
class RenderPass;

namespace internal {
// Declared here so the friend declarations below can name them. Defined in
// atlas/rhi/internal/sdl_gpu_access.hpp, whose only permitted consumer is atlas::tools.
// NOLINTBEGIN(readability-redundant-declaration)
[[nodiscard]] SDL_GPUDevice* native_device(const Device& device) noexcept;
[[nodiscard]] SDL_GPUCommandBuffer* native_command_buffer(const Frame& frame) noexcept;
[[nodiscard]] SDL_GPURenderPass* native_render_pass(const RenderPass& pass) noexcept;
[[nodiscard]] SDL_GPUCommandBuffer* native_command_buffer_of(const RenderPass& pass) noexcept;
[[nodiscard]] unsigned int swapchain_texture_format(const Device& device) noexcept;
// NOLINTEND(readability-redundant-declaration)
}  // namespace internal

/// Records draw commands into one render pass.
///
/// Ends itself on destruction, so the pass is closed even on an early return.
class RenderPass {
  public:
    ~RenderPass();

    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;
    RenderPass(RenderPass&& other) noexcept;
    RenderPass& operator=(RenderPass&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return m_impl != nullptr; }

    void bind_pipeline(GraphicsPipelineHandle pipeline);

    /// Bind a vertex buffer to one stream slot, matching the pipeline's vertex layout.
    void bind_vertex_buffer(std::uint32_t slot, BufferHandle buffer, std::uint64_t offset = 0);

    /// Bind textures and their samplers for the fragment stage, starting at `first_slot`.
    ///
    /// The count must match what the fragment shader declares, which is why the shader's
    /// resource counts are read out of the compiled shader rather than written by hand.
    void bind_fragment_samplers(std::uint32_t first_slot,
                                std::span<const TextureSamplerBinding> bindings);

    /// Push uniform data for the vertex stage.
    ///
    /// The data is copied into the command buffer rather than into a buffer the caller
    /// manages, so it may change every draw without any synchronisation. Sizes are small by
    /// design: SDL pushes these through a fast path meant for a matrix or a handful of
    /// values, not for bulk data.
    void set_vertex_uniforms(std::uint32_t slot, std::span<const std::byte> data);
    void set_fragment_uniforms(std::uint32_t slot, std::span<const std::byte> data);

    /// Restrict drawing to part of the target, in pixels.
    void set_viewport(float x, float y, float width, float height);

    /// Size of the texture this pass draws into, in pixels.
    ///
    /// The swapchain image's size for a swapchain pass, the named texture's for an
    /// offscreen one. A caller setting a viewport, or mapping a pointer position into
    /// target space, needs this and must not assume the window's size.
    [[nodiscard]] Extent2D target_extent() const noexcept;

    /// Format of the texture this pass draws into. A bound pipeline must match it.
    [[nodiscard]] TextureFormat target_format() const noexcept;

    void draw(std::uint32_t vertex_count, std::uint32_t instance_count = 1,
              std::uint32_t first_vertex = 0, std::uint32_t first_instance = 0);

    /// Open a labelled region, for a graphics debugger's timeline.
    void push_debug_group(std::string_view label);
    void pop_debug_group();
    void insert_debug_label(std::string_view label);

    /// Close the pass early. The destructor does this anyway.
    void end();

    /// Opaque implementation state. Declared here rather than privately so that the
    /// implementation's own translation units can define and name it; it is incomplete to
    /// everyone else, so nothing about the backend escapes.
    struct Impl;

  private:
    friend class Frame;
    friend SDL_GPURenderPass* internal::native_render_pass(const RenderPass& pass) noexcept;
    friend SDL_GPUCommandBuffer*
    internal::native_command_buffer_of(const RenderPass& pass) noexcept;

    explicit RenderPass(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

/// One frame's worth of recorded work.
///
/// Acquired from the device, submitted back to it. A frame that is destroyed without being
/// submitted cancels, which is what should happen when a frame is abandoned partway through
/// because of an error.
class Frame {
  public:
    ~Frame();

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&& other) noexcept;
    Frame& operator=(Frame&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return m_impl != nullptr; }

    /// Whether this frame has a swapchain image to draw into.
    ///
    /// False when the window is minimised or the swapchain could not be acquired. A caller
    /// must skip rendering rather than treat it as an error: minimising a window is not a
    /// failure.
    [[nodiscard]] bool has_swapchain_target() const noexcept;

    /// Size of the swapchain image, in pixels.
    [[nodiscard]] Extent2D swapchain_extent() const noexcept;
    [[nodiscard]] TextureFormat swapchain_format() const noexcept;

    /// Begin a render pass against the swapchain image.
    ///
    /// Fails if there is no swapchain target, so check has_swapchain_target() first.
    [[nodiscard]] Result<RenderPass> begin_render_pass(const RenderPassDesc& desc);

    /// Opaque implementation state; see the note on RenderPass::Impl.
    struct Impl;

  private:
    friend class Device;
    friend SDL_GPUCommandBuffer* internal::native_command_buffer(const Frame& frame) noexcept;

    explicit Frame(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

class Device {
  public:
    /// Create a device that presents to `window`.
    ///
    /// The window must outlive the device. Fails with GpuUnavailable when no backend can be
    /// created, which is the normal outcome on a machine with no display.
    [[nodiscard]] static Result<Device> create(const DeviceDesc& desc,
                                               const platform::Window& window);

    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return m_impl != nullptr; }

    [[nodiscard]] Backend backend() const noexcept;

    /// Name the backend reports for itself, for logs and bug reports.
    [[nodiscard]] std::string_view backend_name() const noexcept;

    /// Whether the graphics device has been lost.
    ///
    /// A device can stop working underneath a running process: a driver resets, a display is
    /// unplugged, a graphics processor hangs or is removed. Once that happens every call
    /// fails, so the first failure to say so latches here and every later call returns
    /// `ErrorCode::DeviceLost` immediately rather than attempting work and failing
    /// differently.
    ///
    /// **Atlas does not recover.** Recovery means recreating the device and every resource on
    /// it, which is out of scope for v0.1 (see docs/PROJECT_CHARTER.md). An application that
    /// sees this should report it and exit, not retry.
    ///
    /// **Detection is backend-dependent.** Reliable on Vulkan, best effort on Direct3D 12,
    /// and unavailable on Metal, which has no device-lost notion that reaches the graphics
    /// library. On Metal this always returns false, even for a device that has in fact gone.
    /// Treat it as a way to fail clearly when the backend tells us, never as a health check.
    [[nodiscard]] bool is_lost() const noexcept;

    /// Why the device was lost, as the graphics library described it. Empty while healthy.
    [[nodiscard]] std::string_view loss_reason() const noexcept;

    /// Shader formats this device accepts. A caller picks the one it has.
    [[nodiscard]] bool supports_shader_format(ShaderFormat format) const noexcept;

    /// Format of the swapchain's colour target, which a pipeline must match.
    [[nodiscard]] TextureFormat swapchain_format() const noexcept;

    /// Begin a frame. Always returns a frame; ask it whether it has a target to draw into.
    [[nodiscard]] Result<Frame> begin_frame();

    /// Submit a frame's work. The frame is consumed either way.
    Status end_frame(Frame&& frame);

    /// Wait until the device has finished everything submitted so far.
    ///
    /// Only for shutdown and for reading results back. Calling it per frame would serialise
    /// the processor against the graphics processor and undo the point of frames in flight.
    Status wait_idle();

    [[nodiscard]] Result<BufferHandle> create_buffer(const BufferDesc& desc);

    /// Copy `data` into a buffer, waiting for the copy to finish.
    ///
    /// Synchronous, and meant for initialisation. A streaming path that does not stall
    /// arrives when something needs to update a buffer every frame.
    [[nodiscard]] Status upload_buffer(BufferHandle buffer, std::span<const std::byte> data,
                                       std::uint64_t offset = 0);

    /// Replace a buffer's contents for the draws recorded after this call, without stalling.
    ///
    /// Unlike `upload_buffer` this does not wait for the copy. It cycles the buffer, so
    /// draws already recorded keep the contents they were recorded against and later draws
    /// read the new ones. That is what makes it safe to call every frame with two frames in
    /// flight, and it is why there is no offset: cycling leaves the whole buffer undefined
    /// until written, so a caller must supply the entire range it intends to read. Patching
    /// part of a buffer is `upload_buffer`'s job, and still stalls.
    ///
    /// Measured before it was written: the wait inside `upload_buffer` averages around 460
    /// microseconds per call, which is roughly three quarters of the cost of submitting ten
    /// thousand quads.
    ///
    /// Ownership: `data` is copied before this returns, so the caller may reuse its storage
    /// immediately. The staging memory belongs to the buffer and goes with it.
    ///
    /// Thread affinity: main thread.
    ///
    /// Failure: `InvalidArgument` for a stale handle or data larger than the buffer;
    /// `ResourceCreationFailed` if staging memory cannot be had; `Internal` if the
    /// submission is refused; `DeviceLost` as everywhere. An empty span is a no-op success,
    /// matching `upload_buffer`.
    [[nodiscard]] Status stream_buffer(BufferHandle buffer, std::span<const std::byte> data);

    void destroy_buffer(BufferHandle buffer);

    [[nodiscard]] Result<TextureHandle> create_texture(const TextureDesc& desc);

    /// Copy tightly packed pixel data into a texture, waiting for the copy to finish.
    ///
    /// Synchronous, like upload_buffer, and meant for initialisation.
    [[nodiscard]] Status upload_texture(TextureHandle texture, std::span<const std::byte> pixels);

    void destroy_texture(TextureHandle texture);

    [[nodiscard]] Result<SamplerHandle> create_sampler(const SamplerDesc& desc);
    void destroy_sampler(SamplerHandle sampler);

    [[nodiscard]] Result<ShaderHandle> create_shader(const ShaderDesc& desc);
    void destroy_shader(ShaderHandle shader);

    [[nodiscard]] Result<GraphicsPipelineHandle>
    create_graphics_pipeline(const GraphicsPipelineDesc& desc);
    void destroy_graphics_pipeline(GraphicsPipelineHandle pipeline);

    /// A copy of a rendered frame, in memory.
    struct Capture {
        Extent2D extent;
        TextureFormat format = TextureFormat::Unknown;
        /// Tightly packed rows, four bytes per pixel in `format`.
        std::vector<std::byte> pixels;
    };

    /// Ask for the next submitted frame's colour target to be copied back to memory.
    ///
    /// The copy happens inside end_frame, because a swapchain image exists only for the
    /// duration of its own frame. It stalls until the copy completes, so this is for
    /// screenshots and tests, never for a frame path. The same readback route carries
    /// integer-ID picking in M7.
    /// A rectangle of pixels copied back from a texture.
    struct Readback {
        /// The region that was asked for, in the source texture.
        Rect2D region;
        TextureFormat format = TextureFormat::Unknown;
        /// Tightly packed rows, `byte_size(format)` bytes per pixel.
        std::vector<std::byte> pixels;
    };

    /// How many readbacks may be outstanding at once.
    ///
    /// Small on purpose. A caller that asks and never collects is a bug, and being told so
    /// at the fifth request is better than growing until memory runs out.
    static constexpr std::size_t kMaxPendingReadbacks = 4;

    /// Ask for a rectangle of a texture to be copied back to memory.
    ///
    /// Records the copy, submits it on its own command buffer, and returns. **It does not
    /// wait.** The cost paid here is one submission, not a pipeline drain, which is what
    /// makes this usable for picking on a frame that also has to be drawn. The pixels
    /// become available a frame or two later: ask `readback_ready`, then take.
    ///
    /// To make a test deterministic, call `wait_idle()` first; after that the copy has
    /// certainly finished.
    ///
    /// Ownership: the device owns the staging memory and the fence until the result is
    /// taken or the device is destroyed. `texture` must stay alive until then.
    ///
    /// Thread affinity: main thread.
    ///
    /// Failure: `InvalidArgument` for a stale handle, an empty region, or a region that
    /// leaves the texture; `NotSupported` for a format with no known pixel size;
    /// `Exhausted` when `kMaxPendingReadbacks` are already outstanding;
    /// `ResourceCreationFailed` if staging memory cannot be had; `DeviceLost` as everywhere.
    [[nodiscard]] Result<ReadbackHandle> request_readback(TextureHandle texture, Rect2D region);

    /// Whether a readback has finished and can be taken.
    ///
    /// Not const: it asks the graphics library and latches the answer, so the fence and the
    /// staging memory are released at the first opportunity rather than the last. False for
    /// a null or stale ticket, which `take_readback` then reports properly.
    [[nodiscard]] bool readback_ready(ReadbackHandle ticket) noexcept;

    /// Take a finished readback. The ticket is released, so a second take fails.
    ///
    /// Failure: `InvalidArgument` for a null or stale ticket; `Unavailable` while the copy
    /// is still in flight, which is not an error — ask `readback_ready` first; `Internal`
    /// if the copy itself failed, in which case the ticket is released so the failure is
    /// reported once rather than on every poll.
    [[nodiscard]] Result<Readback> take_readback(ReadbackHandle ticket);

    /// Readbacks asked for and not yet collected.
    [[nodiscard]] std::size_t pending_readbacks() const noexcept;

    void request_capture() noexcept;

    /// Take the capture, if one completed. Clears it, so a second call returns nothing.
    [[nodiscard]] std::optional<Capture> take_capture() noexcept;

    /// Counts of live resources, for the leak report and for tests.
    struct ResourceCounts {
        std::size_t buffers = 0;
        std::size_t textures = 0;
        std::size_t samplers = 0;
        std::size_t shaders = 0;
        std::size_t pipelines = 0;

        [[nodiscard]] std::size_t total() const noexcept {
            return buffers + textures + samplers + shaders + pipelines;
        }
    };

    [[nodiscard]] ResourceCounts resource_counts() const noexcept;

    /// Opaque implementation state; see the note on RenderPass::Impl.
    struct Impl;

  private:
    friend SDL_GPUDevice* internal::native_device(const Device& device) noexcept;
    friend unsigned int internal::swapchain_texture_format(const Device& device) noexcept;

    explicit Device(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

}  // namespace atlas::rhi
