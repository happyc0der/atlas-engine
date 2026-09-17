// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdl_gamepadmap.hpp"

#include <array>
#include <utility>

namespace atlas::platform::detail {
namespace {

/// The single source of truth for the button mapping, in both directions.
///
/// A table rather than two switch statements: two switches drift apart, and the drift is
/// invisible until a button stops working. The bijection test walks this table.
constexpr std::array kButtonTable = std::to_array<std::pair<GamepadButton, SDL_GamepadButton>>({
    {GamepadButton::South, SDL_GAMEPAD_BUTTON_SOUTH},
    {GamepadButton::East, SDL_GAMEPAD_BUTTON_EAST},
    {GamepadButton::West, SDL_GAMEPAD_BUTTON_WEST},
    {GamepadButton::North, SDL_GAMEPAD_BUTTON_NORTH},
    {GamepadButton::Back, SDL_GAMEPAD_BUTTON_BACK},
    {GamepadButton::Guide, SDL_GAMEPAD_BUTTON_GUIDE},
    {GamepadButton::Start, SDL_GAMEPAD_BUTTON_START},
    {GamepadButton::LeftStick, SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {GamepadButton::RightStick, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {GamepadButton::LeftShoulder, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {GamepadButton::RightShoulder, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {GamepadButton::DpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP},
    {GamepadButton::DpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {GamepadButton::DpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {GamepadButton::DpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
});

constexpr std::array kAxisTable = std::to_array<std::pair<GamepadAxis, SDL_GamepadAxis>>({
    {GamepadAxis::LeftX, SDL_GAMEPAD_AXIS_LEFTX},
    {GamepadAxis::LeftY, SDL_GAMEPAD_AXIS_LEFTY},
    {GamepadAxis::RightX, SDL_GAMEPAD_AXIS_RIGHTX},
    {GamepadAxis::RightY, SDL_GAMEPAD_AXIS_RIGHTY},
    {GamepadAxis::LeftTrigger, SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
    {GamepadAxis::RightTrigger, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
});

}  // namespace

SDL_GamepadButton to_sdl_button(GamepadButton button) noexcept {
    for (const auto& [ours, theirs] : kButtonTable) {
        if (ours == button) {
            return theirs;
        }
    }
    return SDL_GAMEPAD_BUTTON_INVALID;
}

GamepadButton from_sdl_button(SDL_GamepadButton button) noexcept {
    for (const auto& [ours, theirs] : kButtonTable) {
        if (theirs == button) {
            return ours;
        }
    }
    return GamepadButton::Count;
}

SDL_GamepadAxis to_sdl_axis(GamepadAxis axis) noexcept {
    for (const auto& [ours, theirs] : kAxisTable) {
        if (ours == axis) {
            return theirs;
        }
    }
    return SDL_GAMEPAD_AXIS_INVALID;
}

GamepadAxis from_sdl_axis(SDL_GamepadAxis axis) noexcept {
    for (const auto& [ours, theirs] : kAxisTable) {
        if (theirs == axis) {
            return ours;
        }
    }
    return GamepadAxis::Count;
}

}  // namespace atlas::platform::detail
