// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/math/camera.hpp>

namespace atlas::math {

void OrthoCamera::set_viewport(float width, float height) noexcept {
    // A zero viewport would divide by zero in every conversion. Ignoring the request keeps
    // the last good size, which is what a window being dragged to nothing should do.
    if (width <= 0.0F || height <= 0.0F) {
        return;
    }
    if (width == m_viewport_width && height == m_viewport_height) {
        return;
    }
    m_viewport_width = width;
    m_viewport_height = height;
    invalidate();
}

void OrthoCamera::set_centre(Vec2 centre) noexcept {
    if (centre == m_centre) {
        return;
    }
    m_centre = centre;
    invalidate();
}

void OrthoCamera::set_zoom(float zoom) noexcept {
    if (zoom <= 0.0F || zoom == m_zoom) {
        return;
    }
    m_zoom = zoom;
    invalidate();
}

void OrthoCamera::zoom_about(float factor, Vec2 anchor_screen) noexcept {
    if (factor <= 0.0F) {
        return;
    }

    // Remember where the anchor was in the world, change the zoom, then move the centre so
    // that the same world point is back under the same pixel.
    const Vec2 before = screen_to_world(anchor_screen);
    set_zoom(m_zoom * factor);
    const Vec2 after = screen_to_world(anchor_screen);
    set_centre(m_centre + (before - after));
}

Vec2 OrthoCamera::screen_to_world(Vec2 screen) const noexcept {
    const Vec2 from_centre{.x = screen.x - (m_viewport_width * 0.5F),
                           .y = screen.y - (m_viewport_height * 0.5F)};
    return m_centre + (from_centre / m_zoom);
}

Vec2 OrthoCamera::world_to_screen(Vec2 world) const noexcept {
    const Vec2 from_centre = (world - m_centre) * m_zoom;
    return {from_centre.x + (m_viewport_width * 0.5F), from_centre.y + (m_viewport_height * 0.5F)};
}

Rect OrthoCamera::visible_bounds() const noexcept {
    const Vec2 size{.x = m_viewport_width / m_zoom, .y = m_viewport_height / m_zoom};
    return Rect{.position = {.x = m_centre.x - (size.x * 0.5F), .y = m_centre.y - (size.y * 0.5F)},
                .size = size};
}

const Mat4& OrthoCamera::view_projection() const noexcept {
    if (m_dirty) {
        const Rect bounds = visible_bounds();

        // top and bottom are swapped relative to the usual mathematical convention, which is
        // what makes y increase downwards in world space.
        m_view_projection =
            orthographic(bounds.left(), bounds.right(), bounds.bottom(), bounds.top(), 0.0F, 1.0F);
        m_dirty = false;
    }
    return m_view_projection;
}

}  // namespace atlas::math
