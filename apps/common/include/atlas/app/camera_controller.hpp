// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/// \file
/// Panning and zooming a camera from a pointer or a gamepad.
///
/// Three applications had character-identical copies of this, and M9 fixed one bug in four
/// places because of it. A gamepad would have made it four copies, so it is one.
///
/// Split in two on purpose. `CameraFrameInput` is plain data describing one frame, and
/// `CameraController::apply` is arithmetic over it, so the arithmetic can be tested without a
/// window system — which matters because only the platform may write an `InputState`, so a
/// test cannot build one. `sample_camera_input` is the thin part that reads the platform and
/// is covered by using the applications rather than by a unit test.
///
/// Thread affinity: main thread, with the platform it samples.

#include <atlas/math/camera.hpp>
#include <atlas/math/vector.hpp>
#include <atlas/platform/event.hpp>
#include <atlas/platform/gamepad.hpp>
#include <atlas/platform/input.hpp>
#include <atlas/platform/key.hpp>

#include <span>

namespace atlas::app {

/// How a camera responds. Defaults are what the applications used before this existed.
struct CameraControls {
    /// The lab drags with the right button because the left one picks.
    platform::MouseButton drag_button = platform::MouseButton::Left;

    /// One wheel notch multiplies the zoom by this.
    float wheel_zoom_base = 1.15F;

    /// Screen pixels a second at full stick deflection, divided by the zoom so that panning
    /// covers the same amount of screen whatever the camera is looking at.
    float stick_pan_pixels_per_second = 600.0F;

    /// Multiplies the zoom by e to this power per second at full deflection, so holding the
    /// axis changes the zoom by a constant proportion rather than a constant amount.
    float stick_zoom_per_second = 1.0F;
};

/// One frame of camera-relevant input, as plain data.
struct CameraFrameInput {
    bool drag_started = false;
    bool drag_ended = false;

    /// Pointer movement this frame, in the window's logical units.
    float mouse_delta_x = 0.0F;
    float mouse_delta_y = 0.0F;

    /// Where the pointer is, in the window's logical units.
    platform::Point2D pointer;

    /// Wheel notches this frame, summed. Summing is right because the zoom is multiplicative:
    /// the product of two notches equals one notch of twice the size.
    float wheel_notches = 0.0F;

    /// Left stick, after the dead zone.
    float pan_x = 0.0F;
    float pan_y = 0.0F;

    /// Right trigger minus left trigger, so pulling both cancels.
    float zoom_axis = 0.0F;
};

/// Read one frame of camera input from the platform.
///
/// `mouse_allowed` is false while the overlay owns the pointer, so a drag on a panel does not
/// also move the world behind it. The gamepad is not gated that way: a controller has no
/// pointer to be over a panel with.
[[nodiscard]] CameraFrameInput sample_camera_input(const platform::InputState& input,
                                                   std::span<const platform::Event> events,
                                                   const CameraControls& controls,
                                                   bool mouse_allowed) noexcept;

class CameraController {
  public:
    explicit CameraController(CameraControls controls = {}) noexcept : m_controls(controls) {}

    /// Apply one frame.
    ///
    /// `display_scale` converts the pointer's logical units into the pixels the camera's
    /// viewport is measured in. Omitting it is the bug M9 fixed in four places: on a
    /// two-times display the camera pans at half speed and zooms about the wrong point.
    ///
    /// `dt_seconds` scales the gamepad rates, which are per second. The pointer contributes
    /// a displacement rather than a rate, so it is not scaled by time.
    void apply(math::OrthoCamera& camera, const CameraFrameInput& frame, float display_scale,
               float dt_seconds) noexcept;

    [[nodiscard]] bool dragging() const noexcept { return m_dragging; }

  private:
    CameraControls m_controls;
    bool m_dragging = false;
};

}  // namespace atlas::app
