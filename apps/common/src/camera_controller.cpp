// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/app/camera_controller.hpp>

#include <cmath>
#include <variant>

namespace atlas::app {

CameraFrameInput sample_camera_input(const platform::InputState& input,
                                     std::span<const platform::Event> events,
                                     const CameraControls& controls, bool mouse_allowed) noexcept {
    CameraFrameInput frame;

    if (mouse_allowed) {
        frame.drag_started = input.was_pressed(controls.drag_button);
        frame.mouse_delta_x = input.mouse_delta_x();
        frame.mouse_delta_y = input.mouse_delta_y();
        frame.pointer = input.mouse_position();
        for (const auto& event : events) {
            if (const auto* wheel = std::get_if<platform::MouseWheel>(&event)) {
                frame.wheel_notches += wheel->delta_y;
            }
        }
    }

    // A release always ends a drag, even while the overlay owns the pointer. Otherwise a drag
    // that finished over a panel would still be running when the pointer came back out.
    frame.drag_ended = input.was_released(controls.drag_button);

    // The first connected pad drives the camera. Which pad is a question for a game with more
    // than one player, and this is an engineering tool.
    for (platform::GamepadId slot = 0; slot < platform::kMaxGamepads; ++slot) {
        if (!input.is_connected(slot)) {
            continue;
        }
        frame.pan_x = input.axis(slot, platform::GamepadAxis::LeftX);
        frame.pan_y = input.axis(slot, platform::GamepadAxis::LeftY);
        frame.zoom_axis = input.axis(slot, platform::GamepadAxis::RightTrigger) -
                          input.axis(slot, platform::GamepadAxis::LeftTrigger);
        break;
    }

    return frame;
}

void CameraController::apply(math::OrthoCamera& camera, const CameraFrameInput& frame,
                             float display_scale, float dt_seconds) noexcept {
    if (frame.drag_started) {
        m_dragging = true;
    }
    if (frame.drag_ended) {
        m_dragging = false;
    }

    const float zoom = camera.zoom();

    if (m_dragging) {
        // The camera moves opposite to the pointer, so the world appears to follow it, and the
        // movement is divided by the zoom so that a drag covers the same distance on screen
        // however far in the camera is.
        camera.pan({-frame.mouse_delta_x * display_scale / zoom,
                    -frame.mouse_delta_y * display_scale / zoom});
    }

    if (frame.wheel_notches != 0.0F) {
        const float factor = std::pow(m_controls.wheel_zoom_base, frame.wheel_notches);
        camera.zoom_about(factor, math::Vec2{.x = frame.pointer.x * display_scale,
                                             .y = frame.pointer.y * display_scale});
    }

    if (frame.pan_x != 0.0F || frame.pan_y != 0.0F) {
        const float distance = m_controls.stick_pan_pixels_per_second * dt_seconds / zoom;
        camera.pan({frame.pan_x * distance, frame.pan_y * distance});
    }

    if (frame.zoom_axis != 0.0F) {
        // About the centre, which needs no anchor: the camera's centre is by definition the
        // world point under the middle of the viewport, and a gamepad has no pointer to zoom
        // about. Exponential so that holding the trigger changes the zoom by a constant
        // proportion per second rather than a constant amount.
        camera.set_zoom(zoom *
                        std::exp(frame.zoom_axis * m_controls.stick_zoom_per_second * dt_seconds));
    }
}

}  // namespace atlas::app
