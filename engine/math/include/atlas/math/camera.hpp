// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// A two-dimensional camera.
///
/// Holds a centre, a zoom and a viewport, and produces the matrix the shaders multiply by.
/// The world is in screen-like coordinates: y increases downwards, because that is what a
/// map, a user interface and every image format already do, and flipping once here is
/// cheaper than flipping at every use.

#include <atlas/math/matrix.hpp>
#include <atlas/math/vector.hpp>

namespace atlas::math {

class OrthoCamera {
  public:
    /// Viewport size in pixels. Zero is refused, because it would divide by zero when
    /// converting between world and screen coordinates.
    void set_viewport(float width, float height) noexcept;

    /// World position at the centre of the view.
    void set_centre(Vec2 centre) noexcept;

    [[nodiscard]] Vec2 centre() const noexcept { return m_centre; }

    /// Pixels per world unit. Larger means closer in. Non-positive values are ignored.
    void set_zoom(float zoom) noexcept;

    [[nodiscard]] float zoom() const noexcept { return m_zoom; }

    void pan(Vec2 delta) noexcept { set_centre(m_centre + delta); }

    /// Multiply the zoom, keeping `anchor` (in screen pixels) over the same world point.
    ///
    /// This is what makes wheel zoom feel right: the world does not slide out from under
    /// the pointer.
    void zoom_about(float factor, Vec2 anchor_screen) noexcept;

    /// World rectangle currently visible.
    [[nodiscard]] Rect visible_bounds() const noexcept;

    [[nodiscard]] Vec2 screen_to_world(Vec2 screen) const noexcept;
    [[nodiscard]] Vec2 world_to_screen(Vec2 world) const noexcept;

    /// The transform to hand the shaders, recomputed only when something changed.
    [[nodiscard]] const Mat4& view_projection() const noexcept;

  private:
    void invalidate() noexcept { m_dirty = true; }

    Vec2 m_centre;
    float m_zoom = 1.0F;
    float m_viewport_width = 1.0F;
    float m_viewport_height = 1.0F;

    mutable Mat4 m_view_projection;
    mutable bool m_dirty = true;
};

}  // namespace atlas::math
