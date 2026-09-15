// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Current input state, sampled once per frame.
///
/// Events say what changed; this says what is true now. Code that needs "is the shift key
/// down" should ask here rather than tracking key events itself, because tracking state
/// from events is where dropped-key bugs come from.
///
/// The pressed and released queries refer to the most recent pump, so they answer "this
/// frame" for a caller that pumps once per frame.

#include <atlas/platform/key.hpp>
#include <atlas/platform/types.hpp>

#include <array>
#include <cstdint>

namespace atlas::platform {

class InputState {
  public:
    [[nodiscard]] bool is_down(Key key) const noexcept;

    /// Went down during the most recent pump.
    [[nodiscard]] bool was_pressed(Key key) const noexcept;

    /// Went up during the most recent pump.
    [[nodiscard]] bool was_released(Key key) const noexcept;

    [[nodiscard]] bool is_down(MouseButton button) const noexcept;
    [[nodiscard]] bool was_pressed(MouseButton button) const noexcept;
    [[nodiscard]] bool was_released(MouseButton button) const noexcept;

    [[nodiscard]] KeyModifiers modifiers() const noexcept { return m_modifiers; }

    /// Pointer position in logical units, relative to the focused window.
    [[nodiscard]] Point2D mouse_position() const noexcept { return m_mouse_position; }

    /// Pointer movement during the most recent pump.
    [[nodiscard]] float mouse_delta_x() const noexcept { return m_mouse_delta_x; }

    [[nodiscard]] float mouse_delta_y() const noexcept { return m_mouse_delta_y; }

    /// Wheel movement during the most recent pump.
    [[nodiscard]] float wheel_delta_x() const noexcept { return m_wheel_delta_x; }

    [[nodiscard]] float wheel_delta_y() const noexcept { return m_wheel_delta_y; }

  private:
    // Platform is the only thing allowed to write input state. A caller that could forge
    // input would make the "events say what changed, this says what is true" split a lie.
    friend class Platform;

    /// Clear the per-frame edges and deltas. Called at the start of each pump.
    void begin_frame() noexcept;

    void set_key(Key key, bool down) noexcept;
    void set_mouse_button(MouseButton button, bool down) noexcept;

    static constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::Count);
    static constexpr std::size_t kButtonCount = static_cast<std::size_t>(MouseButton::Count);

    std::array<bool, kKeyCount> m_key_down{};
    std::array<bool, kKeyCount> m_key_pressed{};
    std::array<bool, kKeyCount> m_key_released{};

    std::array<bool, kButtonCount> m_button_down{};
    std::array<bool, kButtonCount> m_button_pressed{};
    std::array<bool, kButtonCount> m_button_released{};

    KeyModifiers m_modifiers;
    Point2D m_mouse_position;
    float m_mouse_delta_x = 0.0F;
    float m_mouse_delta_y = 0.0F;
    float m_wheel_delta_x = 0.0F;
    float m_wheel_delta_y = 0.0F;
};

}  // namespace atlas::platform
