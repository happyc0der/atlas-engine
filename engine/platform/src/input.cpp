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

}  // namespace atlas::platform
