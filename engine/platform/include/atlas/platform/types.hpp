// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Small value types shared by the platform API.

#include <cstdint>

namespace atlas::platform {

/// A size in pixels or in logical units. Which one is always stated by the accessor that
/// returns it, because confusing the two is the standard high-DPI bug.
struct Extent2D {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] friend constexpr bool operator==(Extent2D, Extent2D) = default;
};

/// A position in logical units, relative to the top-left of a window.
struct Point2D {
    float x = 0.0F;
    float y = 0.0F;
};

/// A rectangle in logical units, matching Point2D rather than pixels.
///
/// Used to tell the window system where a text caret is, so an input method can put its
/// candidate list beside it rather than over it.
struct Rect2D {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;

    [[nodiscard]] friend bool operator==(const Rect2D&, const Rect2D&) = default;
};

}  // namespace atlas::platform
