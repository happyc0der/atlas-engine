// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Small fixed-size vectors.
///
/// First-party rather than a library: Atlas needs a handful of operations on two, three and
/// four floats, and a dependency here would put a third-party type in public headers for no
/// benefit. If the geometry gets hard enough to want quaternions and decompositions, that is
/// the moment to reconsider, and it has not arrived.
///
/// These are presentation types. Simulation state is integer or fixed-point; see
/// docs/DETERMINISM.md.

#include <cmath>
#include <cstddef>

namespace atlas::math {

struct Vec2 {
    float x = 0.0F;
    float y = 0.0F;

    [[nodiscard]] friend constexpr bool operator==(Vec2, Vec2) noexcept = default;

    constexpr Vec2& operator+=(Vec2 other) noexcept {
        x += other.x;
        y += other.y;
        return *this;
    }

    constexpr Vec2& operator-=(Vec2 other) noexcept {
        x -= other.x;
        y -= other.y;
        return *this;
    }

    constexpr Vec2& operator*=(float scalar) noexcept {
        x *= scalar;
        y *= scalar;
        return *this;
    }
};

[[nodiscard]] constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept {
    return {a.x + b.x, a.y + b.y};
}

[[nodiscard]] constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept {
    return {a.x - b.x, a.y - b.y};
}

[[nodiscard]] constexpr Vec2 operator-(Vec2 v) noexcept {
    return {-v.x, -v.y};
}

[[nodiscard]] constexpr Vec2 operator*(Vec2 v, float s) noexcept {
    return {v.x * s, v.y * s};
}

[[nodiscard]] constexpr Vec2 operator*(float s, Vec2 v) noexcept {
    return v * s;
}

[[nodiscard]] constexpr Vec2 operator/(Vec2 v, float s) noexcept {
    return {v.x / s, v.y / s};
}

[[nodiscard]] constexpr float dot(Vec2 a, Vec2 b) noexcept {
    return (a.x * b.x) + (a.y * b.y);
}

[[nodiscard]] constexpr float length_squared(Vec2 v) noexcept {
    return dot(v, v);
}

[[nodiscard]] inline float length(Vec2 v) noexcept {
    return std::sqrt(length_squared(v));
}

/// Unit vector, or the zero vector when the input has no direction to preserve.
[[nodiscard]] inline Vec2 normalise(Vec2 v) noexcept {
    const float len = length(v);
    return len > 0.0F ? v / len : Vec2{};
}

struct Vec3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;

    [[nodiscard]] friend constexpr bool operator==(Vec3, Vec3) noexcept = default;
};

[[nodiscard]] constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept {
    return {.x = a.x + b.x, .y = a.y + b.y, .z = a.z + b.z};
}

[[nodiscard]] constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept {
    return {.x = a.x - b.x, .y = a.y - b.y, .z = a.z - b.z};
}

[[nodiscard]] constexpr Vec3 operator*(Vec3 v, float s) noexcept {
    return {.x = v.x * s, .y = v.y * s, .z = v.z * s};
}

[[nodiscard]] constexpr float dot(Vec3 a, Vec3 b) noexcept {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

struct Vec4 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 0.0F;

    [[nodiscard]] friend constexpr bool operator==(Vec4, Vec4) noexcept = default;

    /// Indexed access, for the matrix code. Out of range is a programming error, and the
    /// callers are all internal, so this does not range-check.
    [[nodiscard]] constexpr float operator[](std::size_t index) const noexcept {
        switch (index) {
        case 0: return x;
        case 1: return y;
        case 2: return z;
        default: return w;
        }
    }
};

[[nodiscard]] constexpr Vec4 operator+(Vec4 a, Vec4 b) noexcept {
    return {.x = a.x + b.x, .y = a.y + b.y, .z = a.z + b.z, .w = a.w + b.w};
}

[[nodiscard]] constexpr Vec4 operator*(Vec4 v, float s) noexcept {
    return {.x = v.x * s, .y = v.y * s, .z = v.z * s, .w = v.w * s};
}

[[nodiscard]] constexpr float dot(Vec4 a, Vec4 b) noexcept {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
}

/// An axis-aligned rectangle, stored as a corner and a size.
///
/// Position plus size rather than two corners: it is what both the quad batcher and the
/// texture atlas want, and storing the other form would mean converting at every use.
struct Rect {
    Vec2 position;
    Vec2 size;

    [[nodiscard]] constexpr float left() const noexcept { return position.x; }

    [[nodiscard]] constexpr float top() const noexcept { return position.y; }

    [[nodiscard]] constexpr float right() const noexcept { return position.x + size.x; }

    [[nodiscard]] constexpr float bottom() const noexcept { return position.y + size.y; }

    [[nodiscard]] constexpr Vec2 centre() const noexcept {
        return {position.x + (size.x * 0.5F), position.y + (size.y * 0.5F)};
    }

    [[nodiscard]] constexpr bool contains(Vec2 point) const noexcept {
        return point.x >= left() && point.x < right() && point.y >= top() && point.y < bottom();
    }

    /// Whether two rectangles share any area. Touching edges do not overlap.
    [[nodiscard]] constexpr bool overlaps(const Rect& other) const noexcept {
        return left() < other.right() && other.left() < right() && top() < other.bottom() &&
               other.top() < bottom();
    }

    [[nodiscard]] friend constexpr bool operator==(const Rect&, const Rect&) noexcept = default;
};

}  // namespace atlas::math
