// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/platform/gamepad.hpp>

namespace atlas::platform {

std::string_view to_string(GamepadButton button) noexcept {
    switch (button) {
    case GamepadButton::South: return "South";
    case GamepadButton::East: return "East";
    case GamepadButton::West: return "West";
    case GamepadButton::North: return "North";
    case GamepadButton::Back: return "Back";
    case GamepadButton::Guide: return "Guide";
    case GamepadButton::Start: return "Start";
    case GamepadButton::LeftStick: return "LeftStick";
    case GamepadButton::RightStick: return "RightStick";
    case GamepadButton::LeftShoulder: return "LeftShoulder";
    case GamepadButton::RightShoulder: return "RightShoulder";
    case GamepadButton::DpadUp: return "DpadUp";
    case GamepadButton::DpadDown: return "DpadDown";
    case GamepadButton::DpadLeft: return "DpadLeft";
    case GamepadButton::DpadRight: return "DpadRight";
    case GamepadButton::Count: return "Count";
    }
    return "Count";
}

std::string_view to_string(GamepadAxis axis) noexcept {
    switch (axis) {
    case GamepadAxis::LeftX: return "LeftX";
    case GamepadAxis::LeftY: return "LeftY";
    case GamepadAxis::RightX: return "RightX";
    case GamepadAxis::RightY: return "RightY";
    case GamepadAxis::LeftTrigger: return "LeftTrigger";
    case GamepadAxis::RightTrigger: return "RightTrigger";
    case GamepadAxis::Count: return "Count";
    }
    return "Count";
}

}  // namespace atlas::platform
