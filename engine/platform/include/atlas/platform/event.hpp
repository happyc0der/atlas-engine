// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Window and input events, as an explicit sum type.
///
/// A variant rather than a tagged struct with a union of payloads: the compiler then
/// enforces that every handler covers every case it claims to, and adding an event breaks
/// exhaustive visitors at compile time rather than silently falling through at runtime.
///
/// Events describe *transitions*. Current state is queried instead, through InputState and
/// the Window accessors. Mixing the two is how input handling becomes order-dependent.

#include <atlas/platform/gamepad.hpp>
#include <atlas/platform/key.hpp>
#include <atlas/platform/types.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <variant>

namespace atlas::platform {

/// Identifies which window an event refers to. Zero means "not window-specific".
using WindowId = std::uint32_t;

/// The user asked the application to quit: the last window closed, or the OS asked.
struct QuitRequested {};

/// A window's close button was pressed. Closing is the application's decision, not SDL's.
struct WindowCloseRequested {
    WindowId window = 0;
};

struct WindowResized {
    WindowId window = 0;
    /// Logical size, in the units window positions use.
    Extent2D size;
    /// Backing-store size, in real pixels. On a Retina display this is larger than `size`,
    /// and it is the one a viewport must use.
    Extent2D pixel_size;
};

struct WindowMinimized {
    WindowId window = 0;
};

struct WindowRestored {
    WindowId window = 0;
};

struct WindowFocusGained {
    WindowId window = 0;
};

struct WindowFocusLost {
    WindowId window = 0;
};

/// The display scale changed, because the window moved to another monitor or the user
/// changed the setting.
struct WindowDisplayScaleChanged {
    WindowId window = 0;
    float scale = 1.0F;
};

struct KeyPressed {
    Key key = Key::Unknown;
    KeyModifiers modifiers;
    /// True when this is the operating system repeating a held key rather than a fresh
    /// press. Text entry wants repeats; a jump button does not.
    bool repeat = false;
};

struct KeyReleased {
    Key key = Key::Unknown;
    KeyModifiers modifiers;
};

struct MouseMoved {
    Point2D position;
    /// Movement since the previous event, which is not the same as the difference between
    /// positions when the pointer is grabbed or warped.
    float delta_x = 0.0F;
    float delta_y = 0.0F;
};

struct MouseButtonPressed {
    MouseButton button = MouseButton::Left;
    Point2D position;
    std::uint8_t clicks = 1;
};

struct MouseButtonReleased {
    MouseButton button = MouseButton::Left;
    Point2D position;
};

struct MouseWheel {
    float delta_x = 0.0F;
    float delta_y = 0.0F;
};

/// Text the user committed, UTF-8 encoded.
///
/// Separate from KeyPressed because they answer different questions. A key tells you which
/// physical key moved; this tells you what characters the user meant, after the keyboard
/// layout, any dead keys, and any input method have had their say. A Japanese commit arrives
/// here as several characters and no key press at all.
///
/// **A commit longer than the capacity arrives as several consecutive events, each cut on a
/// code-point boundary.** Concatenating them in order restores the original, which is lossless
/// because committed text is a stream: every consumer appends. The alternative was a payload
/// that owns memory, which would make Event non-trivially-copyable and allocate inside pump().
struct TextInput {
    /// Chosen so the whole event stays small enough to keep the event buffer cheap. Long
    /// enough that a commit from any ordinary keystroke or input method fits in one.
    static constexpr std::size_t kCapacity = 63;

    /// Always NUL-terminated at `length`, so `c_str()` is safe for a C interface.
    std::array<char, kCapacity + 1> bytes{};
    std::uint8_t length = 0;

    [[nodiscard]] std::string_view text() const noexcept {
        return std::string_view{bytes.data(), length};
    }

    [[nodiscard]] const char* c_str() const noexcept { return bytes.data(); }
};

/// An input method's composition in progress: what the user is typing but has not committed.
///
/// **Replaces the previous composition rather than adding to it**, which is why this is not
/// split the way TextInput is. A composition is state, not a stream, and two halves of one
/// preedit would contradict each other. A composition too long for the buffer is truncated and
/// says so, because a consumer showing a preview wants "something is being composed" more than
/// it wants the exact bytes.
struct TextEditing {
    std::array<char, TextInput::kCapacity + 1> bytes{};
    std::uint8_t length = 0;
    bool truncated = false;

    /// The selected range within the composition, in code points. Negative when the input
    /// method does not say, which several do not.
    std::int32_t selection_start = -1;
    std::int32_t selection_length = -1;

    [[nodiscard]] std::string_view text() const noexcept {
        return std::string_view{bytes.data(), length};
    }
};

/// A gamepad was plugged in, or was already present at startup, and took a slot.
struct GamepadConnected {
    GamepadId gamepad = 0;
};

/// A gamepad went away and freed its slot. Anything it was holding is released.
struct GamepadDisconnected {
    GamepadId gamepad = 0;
};

struct GamepadButtonPressed {
    GamepadId gamepad = 0;
    GamepadButton button = GamepadButton::South;
};

struct GamepadButtonReleased {
    GamepadId gamepad = 0;
    GamepadButton button = GamepadButton::South;
};

/// There is deliberately no axis event. A stick's position is state, not a transition, and a
/// resting stick jitters: reporting every change would be hundreds of events a second, which
/// is what would first exhaust the frame's event storage and start allocating. Both consumers
/// ask `InputState` instead.

using Event =
    std::variant<QuitRequested, WindowCloseRequested, WindowResized, WindowMinimized,
                 WindowRestored, WindowFocusGained, WindowFocusLost, WindowDisplayScaleChanged,
                 KeyPressed, KeyReleased, TextInput, TextEditing, MouseMoved, MouseButtonPressed,
                 MouseButtonReleased, MouseWheel, GamepadConnected, GamepadDisconnected,
                 GamepadButtonPressed, GamepadButtonReleased>;

/// Name of the alternative an event currently holds. For logging and debugging.
[[nodiscard]] std::string_view event_name(const Event& event) noexcept;

}  // namespace atlas::platform
