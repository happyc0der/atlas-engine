// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// A four-by-four matrix, and the projections the renderer needs.
///
/// **Storage and convention.** Elements are held row-major: `m[(row * 4) + column]`, and a
/// transform is applied as `matrix * column_vector`. That is the convention the shaders use,
/// because HLSL's `mul(matrix, vector)` means the same thing, and the reflection of the
/// compiled shader confirms the constant buffer is laid out row-major.
///
/// Getting this wrong produces a transposed transform, which looks like geometry in roughly
/// the right place and subtly wrong, so it is not left to inference: `to_string` prints the
/// layout, the unit tests check known products, and a rendered-pixel test puts a quad at a
/// known position and reads back where it landed.

#include <atlas/math/vector.hpp>

#include <array>
#include <cstddef>
#include <string>

namespace atlas::math {

class Mat4 {
  public:
    /// Identity.
    constexpr Mat4() noexcept
        : m_elements{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                     0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F} {}

    /// From sixteen elements in row-major order, reading left to right, top to bottom.
    explicit constexpr Mat4(const std::array<float, 16>& elements) noexcept
        : m_elements(elements) {}

    [[nodiscard]] static constexpr Mat4 identity() noexcept { return {}; }

    [[nodiscard]] static constexpr Mat4 zero() noexcept { return Mat4{std::array<float, 16>{}}; }

    [[nodiscard]] constexpr float at(std::size_t row, std::size_t column) const noexcept {
        return m_elements[(row * 4) + column];
    }

    constexpr void set(std::size_t row, std::size_t column, float value) noexcept {
        m_elements[(row * 4) + column] = value;
    }

    /// Raw elements, row-major: the order this class stores and documents.
    [[nodiscard]] constexpr const std::array<float, 16>& elements() const noexcept {
        return m_elements;
    }

    /// The same matrix in the order a shader uniform expects.
    ///
    /// Shading languages read a matrix from a uniform buffer column-major: the generated
    /// Metal for Atlas's own shaders is `matrix * vector`, which takes consecutive elements
    /// as columns. Atlas stores row-major because that is how a matrix is written down and
    /// how the tests read. The conversion is therefore real, and naming it is the point:
    /// uploading `elements()` by mistake produces a transposed transform, which draws
    /// geometry in roughly the right place with the translation silently dropped.
    [[nodiscard]] constexpr std::array<float, 16> uniform_elements() const noexcept {
        std::array<float, 16> out{};
        for (std::size_t row = 0; row < 4; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
                out[(column * 4) + row] = at(row, column);
            }
        }
        return out;
    }

    [[nodiscard]] friend constexpr bool operator==(const Mat4&, const Mat4&) noexcept = default;

    /// Transform a vector: `matrix * column_vector`, matching the shaders.
    [[nodiscard]] constexpr Vec4 operator*(const Vec4& v) const noexcept {
        return Vec4{
            .x = (at(0, 0) * v.x) + (at(0, 1) * v.y) + (at(0, 2) * v.z) + (at(0, 3) * v.w),
            .y = (at(1, 0) * v.x) + (at(1, 1) * v.y) + (at(1, 2) * v.z) + (at(1, 3) * v.w),
            .z = (at(2, 0) * v.x) + (at(2, 1) * v.y) + (at(2, 2) * v.z) + (at(2, 3) * v.w),
            .w = (at(3, 0) * v.x) + (at(3, 1) * v.y) + (at(3, 2) * v.z) + (at(3, 3) * v.w),
        };
    }

    /// Compose: the right-hand transform is applied first.
    [[nodiscard]] constexpr Mat4 operator*(const Mat4& other) const noexcept {
        Mat4 result = Mat4::zero();
        for (std::size_t row = 0; row < 4; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
                float sum = 0.0F;
                for (std::size_t k = 0; k < 4; ++k) {
                    sum += at(row, k) * other.at(k, column);
                }
                result.set(row, column, sum);
            }
        }
        return result;
    }

    [[nodiscard]] constexpr Mat4 transposed() const noexcept {
        Mat4 result = Mat4::zero();
        for (std::size_t row = 0; row < 4; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
                // Swapped on purpose: that is what transposing is.
                // NOLINTNEXTLINE(readability-suspicious-call-argument)
                result.set(column, row, at(row, column));
            }
        }
        return result;
    }

    /// Rows, one per line, for a failing test or a log.
    [[nodiscard]] std::string to_string() const;

  private:
    std::array<float, 16> m_elements;
};

/// Translation.
[[nodiscard]] constexpr Mat4 translation(Vec2 offset) noexcept {
    Mat4 result;
    result.set(0, 3, offset.x);
    result.set(1, 3, offset.y);
    return result;
}

/// Scale about the origin.
[[nodiscard]] constexpr Mat4 scale(Vec2 factor) noexcept {
    Mat4 result;
    result.set(0, 0, factor.x);
    result.set(1, 1, factor.y);
    return result;
}

/// An orthographic projection onto clip space.
///
/// Clip space here is the one Vulkan, Metal and Direct3D share: x and y in [-1, 1], and
/// **z in [0, 1]**, not the [-1, 1] that OpenGL uses. Using the OpenGL form would put half
/// the depth range behind the near plane, which looks like geometry vanishing for no reason.
///
/// The y axis is not flipped here. Callers that want screen coordinates, with y increasing
/// downwards, pass `top` above `bottom`; see OrthoCamera.
[[nodiscard]] constexpr Mat4 orthographic(float left, float right, float bottom, float top,
                                          float near_plane, float far_plane) noexcept {
    Mat4 result = Mat4::zero();

    const float width = right - left;
    const float height = top - bottom;
    const float depth = far_plane - near_plane;

    result.set(0, 0, 2.0F / width);
    result.set(1, 1, 2.0F / height);
    result.set(2, 2, 1.0F / depth);

    result.set(0, 3, -(right + left) / width);
    result.set(1, 3, -(top + bottom) / height);
    result.set(2, 3, -near_plane / depth);
    result.set(3, 3, 1.0F);

    return result;
}

}  // namespace atlas::math
