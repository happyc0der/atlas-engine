// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/rhi/internal/sdl_gpu_access.hpp>

#include "device_impl.hpp"
#include "sdl_gpu_conv.hpp"

namespace atlas::rhi::internal {

SDL_GPUDevice* native_device(const Device& device) noexcept {
    return device.m_impl != nullptr ? device.m_impl->device : nullptr;
}

SDL_GPUCommandBuffer* native_command_buffer(const Frame& frame) noexcept {
    return frame.m_impl != nullptr ? frame.m_impl->commands : nullptr;
}

SDL_GPURenderPass* native_render_pass(const RenderPass& pass) noexcept {
    if (pass.m_impl == nullptr || pass.m_impl->ended) {
        return nullptr;
    }
    return pass.m_impl->pass;
}

SDL_GPUCommandBuffer* native_command_buffer_of(const RenderPass& pass) noexcept {
    if (pass.m_impl == nullptr || pass.m_impl->ended || pass.m_impl->frame == nullptr) {
        return nullptr;
    }
    return pass.m_impl->frame->commands;
}

unsigned int swapchain_texture_format(const Device& device) noexcept {
    if (device.m_impl == nullptr) {
        return 0;
    }
    return static_cast<unsigned int>(detail::to_sdl(device.m_impl->swapchain_format));
}

}  // namespace atlas::rhi::internal
