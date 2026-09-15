// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Ownership of the windowing system, the event queue, and input.
///
/// One Platform exists per process, it owns the lifetime of the underlying library, and
/// everything it hands out must be destroyed before it is. Construction marks the calling
/// thread as the main thread; every platform and window call afterwards asserts it is on
/// that thread.
///
/// Headless is a configuration, not a separate implementation. A run with `video = false`
/// initialises no video subsystem at all, and window creation then fails with a clear
/// error rather than returning a stub that silently does nothing. There is no NullPlatform,
/// because a second implementation that exists only to be empty is a maintenance cost with
/// no user.

#include <atlas/core/result.hpp>
#include <atlas/platform/event.hpp>
#include <atlas/platform/input.hpp>
#include <atlas/platform/window.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::platform {

struct PlatformConfig {
    /// Initialise the video subsystem. False for a headless run: no display is opened, no
    /// window can be created, and the process works on a machine with no display at all.
    bool video = true;

    /// Ask for a specific video driver. Empty lets the system choose.
    ///
    /// The value that matters is "dummy", which gives a real window and event pipeline with
    /// no display behind it. That is how window code is exercised in continuous integration,
    /// where there is no display to open.
    std::string_view video_driver;

    /// Application name, used by the window system where it shows one.
    std::string_view app_name = "Atlas";
};

class Platform {
  public:
    [[nodiscard]] static Result<Platform> create(const PlatformConfig& config);

    ~Platform();

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;
    Platform(Platform&& other) noexcept;
    Platform& operator=(Platform&& other) noexcept;

    [[nodiscard]] bool has_video() const noexcept { return m_video; }

    /// The video driver actually in use, which may differ from the one requested. Empty
    /// when running without video.
    [[nodiscard]] std::string_view video_driver() const noexcept { return m_video_driver; }

    /// Not const, although it touches no member: creating a window mutates the window
    /// system's own state, and the platform will track the windows it has handed out.
    // NOLINTNEXTLINE(readability-make-member-function-const)
    [[nodiscard]] Result<Window> create_window(const WindowDesc& desc);

    /// Drain the operating system's event queue, update input state, and return the
    /// translated events.
    ///
    /// The returned span is valid until the next call. The backing storage is reused, so a
    /// steady-state frame performs no allocation.
    [[nodiscard]] std::span<const Event> pump();

    [[nodiscard]] const InputState& input() const noexcept { return m_input; }

    /// Whether a quit was requested at any point since creation. Convenience for a main
    /// loop that does not want to inspect every event itself.
    [[nodiscard]] bool quit_requested() const noexcept { return m_quit_requested; }

  private:
    Platform() = default;

    bool m_initialised = false;
    bool m_video = false;
    bool m_quit_requested = false;
    std::string m_video_driver;
    std::vector<Event> m_events;
    InputState m_input;
};

}  // namespace atlas::platform
