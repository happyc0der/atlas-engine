// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/platform/input.hpp>

#include <algorithm>
#include <cstddef>

namespace atlas::platform {
namespace {

[[nodiscard]] constexpr std::size_t index_of(Key key) noexcept {
    return static_cast<std::size_t>(key);
}

[[nodiscard]] constexpr std::size_t index_of(MouseButton button) noexcept {
    return static_cast<std::size_t>(button);
}

[[nodiscard]] constexpr bool in_range(Key key) noexcept {
    return index_of(key) < static_cast<std::size_t>(Key::Count);
}

[[nodiscard]] constexpr bool in_range(MouseButton button) noexcept {
    return index_of(button) < static_cast<std::size_t>(MouseButton::Count);
}

}  // namespace

bool InputState::is_down(Key key) const noexcept {
    return in_range(key) && m_key_down[index_of(key)];
}

bool InputState::was_pressed(Key key) const noexcept {
    return in_range(key) && m_key_pressed[index_of(key)];
}

bool InputState::was_released(Key key) const noexcept {
    return in_range(key) && m_key_released[index_of(key)];
}

bool InputState::is_down(MouseButton button) const noexcept {
    return in_range(button) && m_button_down[index_of(button)];
}

bool InputState::was_pressed(MouseButton button) const noexcept {
    return in_range(button) && m_button_pressed[index_of(button)];
}

bool InputState::was_released(MouseButton button) const noexcept {
    return in_range(button) && m_button_released[index_of(button)];
}

void InputState::begin_frame() noexcept {
    // Held state persists across frames; edges and deltas do not. Forgetting this is how a
    // key appears to be pressed forever.
    m_key_pressed.fill(false);
    m_key_released.fill(false);
    m_button_pressed.fill(false);
    m_button_released.fill(false);
    m_mouse_delta_x = 0.0F;
    m_mouse_delta_y = 0.0F;
    m_wheel_delta_x = 0.0F;
    m_wheel_delta_y = 0.0F;

    // Gamepad edges clear with everything else. Axes deliberately do not: a stick held still
    // is still deflected, and a caller that polls it every frame must keep reading it.
    for (GamepadState& pad : m_gamepads) {
        pad.pressed.fill(false);
        pad.released.fill(false);
    }
}

void InputState::set_key(Key key, bool down) noexcept {
    if (!in_range(key)) {
        return;
    }
    const std::size_t index = index_of(key);

    // Only a change of state is an edge. An operating-system key repeat arrives as another
    // press, and reporting that as a fresh press would make a held key fire every frame.
    if (down && !m_key_down[index]) {
        m_key_pressed[index] = true;
    } else if (!down && m_key_down[index]) {
        m_key_released[index] = true;
    }
    m_key_down[index] = down;
}

void InputState::set_mouse_button(MouseButton button, bool down) noexcept {
    if (!in_range(button)) {
        return;
    }
    const std::size_t index = index_of(button);
    if (down && !m_button_down[index]) {
        m_button_pressed[index] = true;
    } else if (!down && m_button_down[index]) {
        m_button_released[index] = true;
    }
    m_button_down[index] = down;
}

namespace {

/// Normalise the window system's raw axis value and apply the dead zone once.
///
/// The raw range is asymmetric — one more step below centre than above — so the negative and
/// positive halves are scaled separately. Without that, full deflection one way reads slightly
/// past one and the other slightly short, and a caller comparing against 1.0 sees a stick that
/// never quite arrives.
///
/// Above the threshold the remainder is rescaled to the full range, so full deflection still
/// reads exactly 1 rather than 1 minus the dead zone.
[[nodiscard]] float normalise_axis(std::int16_t raw, float dead_zone) noexcept {
    constexpr float kPositiveRange = 32767.0F;
    constexpr float kNegativeRange = 32768.0F;
    const float value = raw < 0 ? static_cast<float>(raw) / kNegativeRange
                                : static_cast<float>(raw) / kPositiveRange;

    const float magnitude = value < 0.0F ? -value : value;
    if (magnitude <= dead_zone) {
        return 0.0F;
    }
    const float scaled = (magnitude - dead_zone) / (1.0F - dead_zone);
    return value < 0.0F ? -scaled : scaled;
}

[[nodiscard]] bool slot_in_range(GamepadId gamepad) noexcept {
    return static_cast<std::size_t>(gamepad) < kMaxGamepads;
}

}  // namespace

bool InputState::is_connected(GamepadId gamepad) const noexcept {
    return slot_in_range(gamepad) && m_gamepads[gamepad].connected;
}

bool InputState::is_down(GamepadId gamepad, GamepadButton button) const noexcept {
    if (!is_connected(gamepad) || button >= GamepadButton::Count) {
        return false;
    }
    return m_gamepads[gamepad].down[static_cast<std::size_t>(button)];
}

bool InputState::was_pressed(GamepadId gamepad, GamepadButton button) const noexcept {
    if (!is_connected(gamepad) || button >= GamepadButton::Count) {
        return false;
    }
    return m_gamepads[gamepad].pressed[static_cast<std::size_t>(button)];
}

bool InputState::was_released(GamepadId gamepad, GamepadButton button) const noexcept {
    if (!is_connected(gamepad) || button >= GamepadButton::Count) {
        return false;
    }
    return m_gamepads[gamepad].released[static_cast<std::size_t>(button)];
}

float InputState::axis(GamepadId gamepad, GamepadAxis axis) const noexcept {
    if (!is_connected(gamepad) || axis >= GamepadAxis::Count) {
        return 0.0F;
    }
    return m_gamepads[gamepad].axis[static_cast<std::size_t>(axis)];
}

void InputState::set_gamepad_connected(GamepadId gamepad, bool connected) noexcept {
    if (!slot_in_range(gamepad)) {
        return;
    }
    // A fresh slot, either way. On disconnect this releases a held stick or button.
    //
    // Deliberately kept although no test can show it matters. While a slot is empty its
    // queries answer false and zero regardless, and when a new pad takes the slot the window
    // system reports its centred axes on open, which overwrites whatever was stale. Both of
    // those are someone else's behaviour: the first is this file's own gating, which a future
    // change could reasonably drop, and the second is undocumented. Clearing costs nothing and
    // does not depend on either.
    m_gamepads[gamepad] = GamepadState{};
    m_gamepads[gamepad].connected = connected;
}

void InputState::set_gamepad_button(GamepadId gamepad, GamepadButton button, bool down) noexcept {
    if (!slot_in_range(gamepad) || button >= GamepadButton::Count) {
        return;
    }
    GamepadState& pad = m_gamepads[gamepad];
    const auto index = static_cast<std::size_t>(button);
    const bool was_down = pad.down[index];
    pad.down[index] = down;
    if (down && !was_down) {
        pad.pressed[index] = true;
    } else if (!down && was_down) {
        pad.released[index] = true;
    }
}

void InputState::set_gamepad_axis(GamepadId gamepad, GamepadAxis axis, std::int16_t raw) noexcept {
    if (!slot_in_range(gamepad) || axis >= GamepadAxis::Count) {
        return;
    }
    const bool is_trigger = axis == GamepadAxis::LeftTrigger || axis == GamepadAxis::RightTrigger;
    m_gamepads[gamepad].axis[static_cast<std::size_t>(axis)] =
        normalise_axis(raw, is_trigger ? kTriggerDeadZone : kStickDeadZone);
}

}  // namespace atlas::platform
