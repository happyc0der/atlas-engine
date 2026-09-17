// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/platform/window.hpp>

#include "sdl_error.hpp"
#include <SDL3/SDL_video.h>

#include <string>
#include <utility>

namespace atlas::platform {
namespace {

[[nodiscard]] SDL_Window* as_sdl(void* handle) noexcept {
    return static_cast<SDL_Window*>(handle);
}

}  // namespace

Window::~Window() {
    if (m_handle != nullptr) {
        ATLAS_ASSERT_MAIN_THREAD();
        SDL_DestroyWindow(as_sdl(m_handle));
        m_handle = nullptr;
    }
}

Window::Window(Window&& other) noexcept
    : m_handle(std::exchange(other.m_handle, nullptr)), m_id(std::exchange(other.m_id, 0)),
      m_title(std::move(other.m_title)) {}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        if (m_handle != nullptr) {
            SDL_DestroyWindow(as_sdl(m_handle));
        }
        m_handle = std::exchange(other.m_handle, nullptr);
        m_id = std::exchange(other.m_id, 0);
        m_title = std::move(other.m_title);
    }
    return *this;
}

Extent2D Window::size() const noexcept {
    if (m_handle == nullptr) {
        return {};
    }
    ATLAS_ASSERT_MAIN_THREAD();

    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSize(as_sdl(m_handle), &width, &height)) {
        return {};
    }
    return Extent2D{.width = static_cast<std::uint32_t>(width < 0 ? 0 : width),
                    .height = static_cast<std::uint32_t>(height < 0 ? 0 : height)};
}

Extent2D Window::pixel_size() const noexcept {
    if (m_handle == nullptr) {
        return {};
    }
    ATLAS_ASSERT_MAIN_THREAD();

    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(as_sdl(m_handle), &width, &height)) {
        return {};
    }
    return Extent2D{.width = static_cast<std::uint32_t>(width < 0 ? 0 : width),
                    .height = static_cast<std::uint32_t>(height < 0 ? 0 : height)};
}

float Window::display_scale() const noexcept {
    if (m_handle == nullptr) {
        return 1.0F;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    const float scale = SDL_GetWindowDisplayScale(as_sdl(m_handle));
    return scale > 0.0F ? scale : 1.0F;
}

bool Window::is_minimized() const noexcept {
    if (m_handle == nullptr) {
        return false;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    // Queried rather than tracked from events. Tracked state can drift out of sync with
    // reality if an event is missed; a query cannot.
    return (SDL_GetWindowFlags(as_sdl(m_handle)) & SDL_WINDOW_MINIMIZED) != 0;
}

bool Window::has_focus() const noexcept {
    if (m_handle == nullptr) {
        return false;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    return (SDL_GetWindowFlags(as_sdl(m_handle)) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

bool Window::is_hidden() const noexcept {
    if (m_handle == nullptr) {
        return true;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    return (SDL_GetWindowFlags(as_sdl(m_handle)) & SDL_WINDOW_HIDDEN) != 0;
}

void Window::set_title(std::string_view title) {
    if (m_handle == nullptr) {
        return;
    }
    ATLAS_ASSERT_MAIN_THREAD();

    m_title.assign(title);
    SDL_SetWindowTitle(as_sdl(m_handle), m_title.c_str());
}

Status Window::set_size(Extent2D size) {
    if (m_handle == nullptr) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "set_size called on an invalid window"));
    }
    ATLAS_ASSERT_MAIN_THREAD();

    if (size.width == 0 || size.height == 0) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument, "window size must have a non-zero width and height"));
    }

    if (!SDL_SetWindowSize(as_sdl(m_handle), static_cast<int>(size.width),
                           static_cast<int>(size.height))) {
        return std::unexpected(
            detail::sdl_error(ErrorCode::Internal, "resizing the window failed"));
    }
    return ok();
}

void Window::show() {
    if (m_handle != nullptr) {
        ATLAS_ASSERT_MAIN_THREAD();
        SDL_ShowWindow(as_sdl(m_handle));
    }
}

void Window::hide() {
    if (m_handle != nullptr) {
        ATLAS_ASSERT_MAIN_THREAD();
        SDL_HideWindow(as_sdl(m_handle));
    }
}

}  // namespace atlas::platform
