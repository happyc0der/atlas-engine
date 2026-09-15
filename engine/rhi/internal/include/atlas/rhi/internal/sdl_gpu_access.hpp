// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The graphics handles behind atlas::rhi, for the one consumer that needs them.
///
/// The debug overlay is drawn by a third-party immediate-mode library whose renderer backend
/// talks to the graphics API directly. It therefore needs the native device, command buffer
/// and render pass that the render hardware interface deliberately hides.
///
/// Rather than widening the public API for that, this lives in a separate target whose only
/// permitted consumer is atlas::tools, recorded in cmake/ModuleGraph.cmake. The types are
/// forward-declared, so including this pulls in no graphics headers.

struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;
struct SDL_GPURenderPass;

namespace atlas::rhi {

class Device;
class Frame;
class RenderPass;

namespace internal {

/// The native device, or nullptr for an invalid one.
[[nodiscard]] SDL_GPUDevice* native_device(const Device& device) noexcept;

/// The command buffer a frame is recording into, or nullptr.
[[nodiscard]] SDL_GPUCommandBuffer* native_command_buffer(const Frame& frame) noexcept;

/// The native render pass, or nullptr if the pass has ended.
[[nodiscard]] SDL_GPURenderPass* native_render_pass(const RenderPass& pass) noexcept;

/// The command buffer the pass is recording into, or nullptr if the pass has ended.
[[nodiscard]] SDL_GPUCommandBuffer* native_command_buffer_of(const RenderPass& pass) noexcept;

/// The swapchain's colour format, in the graphics library's own enumeration.
///
/// Returned as the underlying integer so that this header names no graphics type. The
/// overlay's backend wants exactly this value.
[[nodiscard]] unsigned int swapchain_texture_format(const Device& device) noexcept;

}  // namespace internal
}  // namespace atlas::rhi
