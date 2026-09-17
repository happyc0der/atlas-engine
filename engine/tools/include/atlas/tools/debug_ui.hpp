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
#include <atlas/edit/history.hpp>
#include <atlas/platform/platform.hpp>
#include <atlas/rhi/device.hpp>
#include <atlas/scene/scene.hpp>

#include <cstdint>
#include <memory>
#include <optional>
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

    /// A scene: its tree on the left, the selected entity's components on the right, with
    /// the ones this panel can edit as widgets.
    ///
    /// **Takes the history, not the scene.** From M5 until M9 this took a `const Scene&`, so
    /// that no widget could become a second path into the scene bypassing the validation
    /// `Scene` performs. That reason has not gone away; it has been satisfied. `edit::History`
    /// exposes its scene only as `const` and changes it only through commands it can undo, and
    /// there is no method on it that yields a mutable `Scene`. So a widget here still cannot
    /// reach the scene any other way, and the compiler still enforces that rather than intent.
    ///
    /// Editing is deliberately narrow: the local position, because it is the one property
    /// worth dragging and it proves the whole path. The other commands exist and are tested;
    /// their widgets arrive one at a time. A rename widget needs text input the platform does
    /// not deliver yet, which `docs/DEFERRED.md` records.
    ///
    /// Selection is the panel's own state, not the scene's, and is remembered across frames.
    /// A selected entity that has since been destroyed is dropped silently.
    void scene_panel(std::string_view title, edit::History& history);

    /// The entity currently selected in the scene panel, if any.
    [[nodiscard]] std::optional<scene::StableId> selected_entity() const noexcept;

    /// Select an entity from outside the panel, or clear the selection with `None`.
    ///
    /// Selection is a property of the view, so something other than a click may set it: a
    /// click in the viewport, a search, or a command line asking for a particular entity.
    /// Setting it does not check that the entity exists; the panel drops a selection that has
    /// gone away on the next frame it draws.
    void select_entity(scene::StableId id) noexcept;

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
