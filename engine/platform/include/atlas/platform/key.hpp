// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Keyboard and mouse identifiers.
///
/// Atlas defines its own key enumeration rather than passing SDL scancodes through. Three
/// reasons: SDL types must not appear in public headers, key identifiers end up in
/// serialized input bindings where a third-party numbering would become a compatibility
/// commitment, and a deliberately small set is easier to bind and to display than the
/// couple of hundred scancodes SDL knows about.
///
/// Values are positions, not codes: they may be renumbered until something serializes them,
/// at which point they become fixed. Nothing serializes them yet.

#include <cstdint>
#include <string_view>

namespace atlas::platform {

enum class Key : std::uint16_t {
    Unknown = 0,

    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,

    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,

    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,

    Escape,
    Enter,
    Space,
    Tab,
    Backspace,
    Delete,
    Insert,
    Home,
    End,
    PageUp,
    PageDown,

    Left,
    Right,
    Up,
    Down,

    LeftShift,
    RightShift,
    LeftControl,
    RightControl,
    LeftAlt,
    RightAlt,
    LeftSuper,
    RightSuper,

    Minus,
    Equals,
    LeftBracket,
    RightBracket,
    Backslash,
    Semicolon,
    Apostrophe,
    Grave,
    Comma,
    Period,
    Slash,

    /// Number of enumerators, for sizing arrays. Not a key.
    Count,
};

enum class MouseButton : std::uint8_t {
    Left = 0,
    Middle,
    Right,
    X1,
    X2,

    /// Number of enumerators, for sizing arrays. Not a button.
    Count,
};

/// Modifier keys held when an input event occurred.
struct KeyModifiers {
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool super = false;
};

/// Stable, human-readable name. For logs and for the future binding UI, not for parsing.
[[nodiscard]] std::string_view to_string(Key key) noexcept;
[[nodiscard]] std::string_view to_string(MouseButton button) noexcept;

}  // namespace atlas::platform
