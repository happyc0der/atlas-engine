// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The debug overlay.
///
/// An internal engineering tool, not a user interface: it exists so that timings, counters
/// and controls are visible while the engine is being built. The immediate-mode library
/// behind it is a private implementation detail, so no type from it appears here.
///
/// Thread affinity: main thread only.

#include <atlas/core/result.hpp>
#include <atlas/platform/platform.hpp>
#include <atlas/rhi/device.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace atlas::tools {

/// One line in the statistics panel.
struct Stat {
    std::string_view label;
    std::string_view value;
};

class DebugUi {
  public:
    [[nodiscard]] static Result<DebugUi> create(rhi::Device& device,
                                                const platform::Window& window);

    ~DebugUi();

    DebugUi(const DebugUi&) = delete;
    DebugUi& operator=(const DebugUi&) = delete;
    DebugUi(DebugUi&& other) noexcept;
    DebugUi& operator=(DebugUi&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return m_impl != nullptr; }

    /// Offer an event to the overlay.
    ///
    /// Returns true when the overlay consumed it, in which case the application should not
    /// also act on it: a click on a panel must not also pan the camera behind it.
    [[nodiscard]] bool handle_event(const platform::Event& event);

    /// Whether the overlay currently wants the mouse or the keyboard.
    [[nodiscard]] bool wants_mouse() const noexcept;
    [[nodiscard]] bool wants_keyboard() const noexcept;

    /// Start a frame of interface building.
    ///
    /// The elapsed time drives the library's own animations, and the pixel size keeps the
    /// overlay matched to the swapchain rather than to the logical window, which differ on
    /// a scaled display.
    void begin_frame(float delta_seconds, std::uint32_t pixel_width, std::uint32_t pixel_height);

    /// A panel of label and value rows.
    void stats_panel(std::string_view title, std::span<const Stat> stats);

    /// Proof that an overlay frame was prepared and is ready to be drawn.
    ///
    /// Drawing takes one of these rather than being callable directly, because the two
    /// steps must happen in the right order and in different places: preparing uploads
    /// vertex data, which begins a copy pass, and a copy pass cannot be nested inside the
    /// render pass that draws the result. Requiring the token makes the wrong order fail to
    /// compile instead of failing at run time inside the graphics library.
    class PreparedFrame {
      public:
        /// Not prepared. Drawing with one of these does nothing, which is what should happen
        /// on a frame where the overlay was never built.
        PreparedFrame() noexcept = default;

        [[nodiscard]] bool valid() const noexcept { return m_ready; }

      private:
        friend class DebugUi;

        explicit PreparedFrame(bool ready) noexcept : m_ready(ready) {}

        bool m_ready = false;
    };

    /// Finish building and upload the overlay's vertex data.
    ///
    /// Must be called **outside** any render pass, after the panels are built.
    [[nodiscard]] PreparedFrame end_frame(rhi::Frame& frame);

    /// Record the overlay's draw commands. Must be called inside a render pass.
    void draw(rhi::RenderPass& pass, const PreparedFrame& prepared);

  private:
    struct Impl;
    explicit DebugUi(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

}  // namespace atlas::tools
