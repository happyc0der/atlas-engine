// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The one sanctioned way to reach the window system handle behind an atlas::platform
/// Window.
///
/// The renderer has to hand a native window to the graphics API, and the platform's public
/// headers deliberately do not expose one. Rather than widening the public API for a single
/// consumer, this header lives in a separate target whose only permitted user is
/// atlas::rhi, recorded in cmake/ModuleGraph.cmake.
///
/// SDL_Window is forward-declared, so including this does not pull in SDL. The caller still
/// needs SDL to do anything with the pointer, which keeps the dependency where it belongs.

struct SDL_Window;

namespace atlas::platform {

class Window;

namespace internal {

/// The native window handle, or nullptr for an invalid window.
///
/// The returned pointer is owned by the Window and is valid only while it is.
[[nodiscard]] SDL_Window* native_handle(const Window& window) noexcept;

}  // namespace internal
}  // namespace atlas::platform
