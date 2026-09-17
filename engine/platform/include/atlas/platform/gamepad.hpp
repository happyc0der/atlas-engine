// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/// \file
/// Gamepad buttons, axes, and the slots gamepads occupy.
///
/// The same shape as `key.hpp`, and for the same reasons. These are positions rather than the
/// window system's codes, so nothing here depends on a third-party header. Values may be
/// renumbered until something serializes them, at which point they are fixed; nothing does yet.
///
/// **Face buttons are named by position, not by letter.** The button in the south position is
/// "A" on one vendor's pad and "B" on another's, and the two vendors disagree about which is
/// confirm. A binding interface shows the letter the attached pad prints; the engine works in
/// positions, which is the only thing the two agree on.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace atlas::platform {

enum class GamepadButton : std::uint8_t {
    South = 0,
    East,
    West,
    North,
    Back,
    Guide,
    Start,
    LeftStick,
    RightStick,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    /// Not a button: the count, and the value an unmapped button maps to. `MouseButton`'s
    /// convention rather than `Key`'s, because there is no useful "unknown button".
    Count,
};

enum class GamepadAxis : std::uint8_t {
    LeftX = 0,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,
    Count,
};

/// Which slot a gamepad occupies, not which device it is.
///
/// The platform assigns the lowest free slot when a pad connects and frees it when the pad
/// goes away, so a slot is stable for one connection and is reused afterwards. The window
/// system's own device identifier never leaves the platform's implementation, because it is
/// not stable across runs and would end up in a saved binding if it were exposed.
using GamepadId = std::uint8_t;

/// How many gamepads are tracked at once.
///
/// Four, because that is what a local multiplayer game asks for and what every console
/// convention assumes. A fifth pad is refused with a warning rather than silently ignored.
inline constexpr std::size_t kMaxGamepads = 4;

/// The fraction of a stick's travel treated as centre.
///
/// Sticks do not return exactly to centre, so a resting pad would otherwise pan the camera
/// forever. Applied once, in the platform, because applying it twice is a real bug and the
/// window system's raw range is asymmetric. Rescaled above the threshold, so full deflection
/// still reads exactly 1.
///
/// Tune with a controller in hand; these become configuration when a caller needs different
/// values, and not before.
inline constexpr float kStickDeadZone = 0.2F;

/// Triggers rest at zero and are not centred, so they need far less.
inline constexpr float kTriggerDeadZone = 0.05F;

[[nodiscard]] std::string_view to_string(GamepadButton button) noexcept;
[[nodiscard]] std::string_view to_string(GamepadAxis axis) noexcept;

}  // namespace atlas::platform
