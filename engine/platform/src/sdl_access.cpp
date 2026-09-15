// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/platform/internal/sdl_access.hpp>
#include <atlas/platform/window.hpp>

namespace atlas::platform::internal {

SDL_Window* native_handle(const Window& window) noexcept {
    // Window stores the handle as void* so that its public header names no SDL type. The
    // cast is the whole reason this function exists, and it lives here rather than at the
    // call site so that there is exactly one of it.
    return static_cast<SDL_Window*>(window.m_handle);
}

}  // namespace atlas::platform::internal
