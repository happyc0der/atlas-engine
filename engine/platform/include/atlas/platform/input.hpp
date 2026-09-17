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

#include <atlas/platform/gamepad.hpp>
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

    /// Whether a gamepad occupies this slot. Out-of-range slots answer false rather than
    /// asserting, because a caller polling four slots every frame should not have to check.
    [[nodiscard]] bool is_connected(GamepadId gamepad) const noexcept;

    [[nodiscard]] bool is_down(GamepadId gamepad, GamepadButton button) const noexcept;
    [[nodiscard]] bool was_pressed(GamepadId gamepad, GamepadButton button) const noexcept;
    [[nodiscard]] bool was_released(GamepadId gamepad, GamepadButton button) const noexcept;

    /// A stick in [-1, 1] or a trigger in [0, 1], after the dead zone.
    ///
    /// **Level state, not an edge**: it survives `begin_frame` and keeps its value until the
    /// pad moves, because a stick held still is still deflected. Reads zero for an empty or
    /// out-of-range slot, so a caller that stops checking connectedness gets no movement
    /// rather than stale movement.
    [[nodiscard]] float axis(GamepadId gamepad, GamepadAxis axis) const noexcept;

  private:
    // Platform is the only thing allowed to write input state. A caller that could forge
    // input would make the "events say what changed, this says what is true" split a lie.
    friend class Platform;

    /// Clear the per-frame edges and deltas. Called at the start of each pump.
    void begin_frame() noexcept;

    void set_key(Key key, bool down) noexcept;
    void set_mouse_button(MouseButton button, bool down) noexcept;

    /// Take or release a slot. Releasing clears everything the pad was holding, so a stick
    /// held at the moment it is unplugged does not pan the camera forever.
    void set_gamepad_connected(GamepadId gamepad, bool connected) noexcept;
    void set_gamepad_button(GamepadId gamepad, GamepadButton button, bool down) noexcept;
    /// Raw, in the window system's own range; the dead zone and the rescale are applied here
    /// so that they are applied exactly once.
    void set_gamepad_axis(GamepadId gamepad, GamepadAxis axis, std::int16_t raw) noexcept;

    static constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::Count);
    static constexpr std::size_t kButtonCount = static_cast<std::size_t>(MouseButton::Count);
    static constexpr std::size_t kGamepadButtonCount =
        static_cast<std::size_t>(GamepadButton::Count);
    static constexpr std::size_t kGamepadAxisCount = static_cast<std::size_t>(GamepadAxis::Count);

    std::array<bool, kKeyCount> m_key_down{};
    std::array<bool, kKeyCount> m_key_pressed{};
    std::array<bool, kKeyCount> m_key_released{};

    std::array<bool, kButtonCount> m_button_down{};
    std::array<bool, kButtonCount> m_button_pressed{};
    std::array<bool, kButtonCount> m_button_released{};

    struct GamepadState {
        bool connected = false;
        std::array<bool, kGamepadButtonCount> down{};
        std::array<bool, kGamepadButtonCount> pressed{};
        std::array<bool, kGamepadButtonCount> released{};
        std::array<float, kGamepadAxisCount> axis{};
    };

    std::array<GamepadState, kMaxGamepads> m_gamepads{};

    KeyModifiers m_modifiers;
    Point2D m_mouse_position;
    float m_mouse_delta_x = 0.0F;
    float m_mouse_delta_y = 0.0F;
    float m_wheel_delta_x = 0.0F;
    float m_wheel_delta_y = 0.0F;
};

}  // namespace atlas::platform
