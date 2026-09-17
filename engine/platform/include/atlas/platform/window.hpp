// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// An operating-system window.
///
/// Created through Platform, because a window cannot outlive the platform that owns the
/// video subsystem, and expressing that through the factory is cheaper than documenting it
/// and hoping.
///
/// Thread affinity: every member must be called on the main thread. Window systems on all
/// three tier-one platforms require this, and the assertions here make a violation fail
/// immediately rather than intermittently.

#include <atlas/core/result.hpp>
#include <atlas/platform/event.hpp>
#include <atlas/platform/types.hpp>

#include <string>
#include <string_view>

struct SDL_Window;

namespace atlas::platform {

class Window;

namespace internal {
// Declared here so that the friend declaration below can name it. The definition and the
// documentation live in atlas/platform/internal/sdl_access.hpp.
// NOLINTNEXTLINE(readability-redundant-declaration)
[[nodiscard]] SDL_Window* native_handle(const Window& window) noexcept;
}  // namespace internal

struct WindowDesc {
    std::string_view title = "Atlas";
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    bool resizable = true;

    /// Ask for a backing store at the display's real pixel density. When true, pixel_size()
    /// may exceed size() and the renderer must use the former.
    bool high_dpi = true;

    /// Create the window hidden, so that the first frame can be prepared before anything is
    /// shown. Call show() when ready.
    bool hidden = false;
};

class Window {
  public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;

    /// False for a default-constructed or moved-from window.
    [[nodiscard]] bool valid() const noexcept { return m_handle != nullptr; }

    /// Identifier used by events to say which window they refer to.
    [[nodiscard]] WindowId id() const noexcept { return m_id; }

    /// Size in logical units. What the user thinks the window is.
    [[nodiscard]] Extent2D size() const noexcept;

    /// Size of the backing store in real pixels. What a viewport must use. Equal to size()
    /// except on a scaled display.
    [[nodiscard]] Extent2D pixel_size() const noexcept;

    /// Ratio of pixel size to logical size, as reported by the display.
    [[nodiscard]] float display_scale() const noexcept;

    /// Queried from the window system rather than tracked from events, so it cannot drift
    /// out of sync with reality if an event is ever missed.
    [[nodiscard]] bool is_minimized() const noexcept;
    [[nodiscard]] bool has_focus() const noexcept;
    [[nodiscard]] bool is_hidden() const noexcept;

    [[nodiscard]] std::string_view title() const noexcept { return m_title; }

    void set_title(std::string_view title);

    [[nodiscard]] Status set_size(Extent2D size);
    void show();
    void hide();

  private:
    friend class Platform;

    /// The renderer needs the native handle, and nothing else does. Granting it to one
    /// named function in a separate internal target is narrower than a public accessor.
    friend SDL_Window* internal::native_handle(const Window& window) noexcept;

    /// Opaque; the concrete window type never appears in a public header. The RHI reaches
    /// the underlying handle through a separate internal target.
    void* m_handle = nullptr;
    WindowId m_id = 0;
    std::string m_title;
};

}  // namespace atlas::platform
