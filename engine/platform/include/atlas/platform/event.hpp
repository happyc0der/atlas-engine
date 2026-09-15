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

#include <atlas/platform/key.hpp>
#include <atlas/platform/types.hpp>

#include <cstdint>
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

using Event = std::variant<QuitRequested, WindowCloseRequested, WindowResized, WindowMinimized,
                           WindowRestored, WindowFocusGained, WindowFocusLost,
                           WindowDisplayScaleChanged, KeyPressed, KeyReleased, MouseMoved,
                           MouseButtonPressed, MouseButtonReleased, MouseWheel>;

/// Name of the alternative an event currently holds. For logging and debugging.
[[nodiscard]] std::string_view event_name(const Event& event) noexcept;

}  // namespace atlas::platform
