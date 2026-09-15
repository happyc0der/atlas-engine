// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/math/matrix.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>

using atlas::math::Mat4;
using atlas::math::orthographic;
using atlas::math::scale;
using atlas::math::translation;
using atlas::math::Vec2;
using atlas::math::Vec4;
using Catch::Approx;

TEST_CASE("the identity leaves a vector alone", "[math][matrix]") {
    constexpr Mat4 identity;
    constexpr Vec4 v{1.0F, 2.0F, 3.0F, 1.0F};
    STATIC_REQUIRE(identity * v == v);
    STATIC_REQUIRE(identity.at(0, 0) == 1.0F);
    STATIC_REQUIRE(identity.at(0, 1) == 0.0F);
}

TEST_CASE("elements are stored row-major", "[math][matrix]") {
    // The convention the shaders rely on. Storing these column-major instead would produce a
    // transposed transform: geometry roughly in the right place and subtly wrong, which is
    // the worst kind of wrong to debug.
    constexpr Mat4 m{std::array<float, 16>{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F, 9.0F,
                                           10.0F, 11.0F, 12.0F, 13.0F, 14.0F, 15.0F, 16.0F}};
    STATIC_REQUIRE(m.at(0, 0) == 1.0F);
    STATIC_REQUIRE(m.at(0, 3) == 4.0F);
    STATIC_REQUIRE(m.at(1, 0) == 5.0F);
    STATIC_REQUIRE(m.at(3, 3) == 16.0F);
    STATIC_REQUIRE(m.elements()[7] == 8.0F);
}

TEST_CASE("a matrix times a vector, computed by hand", "[math][matrix]") {
    // Row zero is (1, 2, 3, 4) and the vector is (1, 1, 1, 1), so the first component must
    // be 1+2+3+4 = 10. Worked out on paper, not read off the implementation.
    constexpr Mat4 m{std::array<float, 16>{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F, 9.0F,
                                           10.0F, 11.0F, 12.0F, 13.0F, 14.0F, 15.0F, 16.0F}};
    constexpr Vec4 ones{1.0F, 1.0F, 1.0F, 1.0F};
    constexpr Vec4 result = m * ones;

    STATIC_REQUIRE(result.x == 10.0F);
    STATIC_REQUIRE(result.y == 26.0F);
    STATIC_REQUIRE(result.z == 42.0F);
    STATIC_REQUIRE(result.w == 58.0F);
}

TEST_CASE("translation moves a point and leaves a direction alone", "[math][matrix]") {
    constexpr Mat4 t = translation(Vec2{5.0F, -3.0F});

    // A point has w = 1 and is moved.
    constexpr Vec4 point = t * Vec4{1.0F, 1.0F, 0.0F, 1.0F};
    STATIC_REQUIRE(point.x == 6.0F);
    STATIC_REQUIRE(point.y == -2.0F);

    // A direction has w = 0 and is not. This is the reason translation lives in the fourth
    // column rather than being added separately.
    constexpr Vec4 direction = t * Vec4{1.0F, 1.0F, 0.0F, 0.0F};
    STATIC_REQUIRE(direction.x == 1.0F);
    STATIC_REQUIRE(direction.y == 1.0F);
}

TEST_CASE("scale multiplies each axis", "[math][matrix]") {
    constexpr Mat4 s = scale(Vec2{2.0F, 3.0F});
    constexpr Vec4 result = s * Vec4{4.0F, 5.0F, 0.0F, 1.0F};
    STATIC_REQUIRE(result.x == 8.0F);
    STATIC_REQUIRE(result.y == 15.0F);
}

TEST_CASE("composition applies the right-hand transform first", "[math][matrix]") {
    constexpr Mat4 t = translation(Vec2{10.0F, 0.0F});
    constexpr Mat4 s = scale(Vec2{2.0F, 2.0F});

    // Scale then translate: (1,0) scales to (2,0), then moves to (12,0).
    constexpr Vec4 scaled_then_moved = (t * s) * Vec4{1.0F, 0.0F, 0.0F, 1.0F};
    STATIC_REQUIRE(scaled_then_moved.x == 12.0F);

    // Translate then scale: (1,0) moves to (11,0), then scales to (22,0). The order matters,
    // and this is what pins down which way round composition works.
    constexpr Vec4 moved_then_scaled = (s * t) * Vec4{1.0F, 0.0F, 0.0F, 1.0F};
    STATIC_REQUIRE(moved_then_scaled.x == 22.0F);
}

TEST_CASE("transpose swaps rows and columns, twice returns the original", "[math][matrix]") {
    constexpr Mat4 m{std::array<float, 16>{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F, 9.0F,
                                           10.0F, 11.0F, 12.0F, 13.0F, 14.0F, 15.0F, 16.0F}};
    STATIC_REQUIRE(m.transposed().at(0, 1) == m.at(1, 0));
    STATIC_REQUIRE(m.transposed().transposed() == m);
}

TEST_CASE("orthographic maps the view rectangle onto clip space", "[math][matrix]") {
    // A 100 by 100 view with the origin at its centre.
    constexpr Mat4 projection = orthographic(-50.0F, 50.0F, -50.0F, 50.0F, 0.0F, 1.0F);

    const auto project = [&](float x, float y) { return projection * Vec4{x, y, 0.0F, 1.0F}; };

    const Vec4 centre = project(0.0F, 0.0F);
    CHECK(centre.x == Approx(0.0F).margin(1e-6));
    CHECK(centre.y == Approx(0.0F).margin(1e-6));

    const Vec4 right = project(50.0F, 0.0F);
    CHECK(right.x == Approx(1.0F));

    const Vec4 left = project(-50.0F, 0.0F);
    CHECK(left.x == Approx(-1.0F));

    const Vec4 top = project(0.0F, 50.0F);
    CHECK(top.y == Approx(1.0F));
}

TEST_CASE("orthographic puts depth in zero to one, not minus one to one", "[math][matrix]") {
    // Vulkan, Metal and Direct3D all use a zero-to-one depth range. Emitting the OpenGL
    // range instead would put half of it behind the near plane, and geometry would vanish
    // for no visible reason.
    constexpr Mat4 projection = orthographic(-1.0F, 1.0F, -1.0F, 1.0F, 0.0F, 1.0F);

    const Vec4 near_plane = projection * Vec4{0.0F, 0.0F, 0.0F, 1.0F};
    const Vec4 far_plane = projection * Vec4{0.0F, 0.0F, 1.0F, 1.0F};

    CHECK(near_plane.z == Approx(0.0F).margin(1e-6));
    CHECK(far_plane.z == Approx(1.0F));
}

TEST_CASE("orthographic handles an off-centre view", "[math][matrix]") {
    // Left 0, right 200, bottom 100, top 0: screen-like, with y increasing downwards.
    constexpr Mat4 projection = orthographic(0.0F, 200.0F, 100.0F, 0.0F, 0.0F, 1.0F);

    const Vec4 top_left = projection * Vec4{0.0F, 0.0F, 0.0F, 1.0F};
    CHECK(top_left.x == Approx(-1.0F));
    CHECK(top_left.y == Approx(1.0F));

    const Vec4 bottom_right = projection * Vec4{200.0F, 100.0F, 0.0F, 1.0F};
    CHECK(bottom_right.x == Approx(1.0F));
    CHECK(bottom_right.y == Approx(-1.0F));
}

TEST_CASE("uniform elements are the transpose of the stored elements", "[math][matrix]") {
    // Shading languages read a uniform matrix column-major; Atlas stores row-major. Getting
    // this backwards produces a transform with the translation silently dropped, which
    // renders geometry in roughly the right place and is miserable to diagnose.
    constexpr Mat4 m{std::array<float, 16>{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F, 9.0F,
                                           10.0F, 11.0F, 12.0F, 13.0F, 14.0F, 15.0F, 16.0F}};
    constexpr auto uniform = m.uniform_elements();

    // First column of the matrix, which is the first four elements a shader will read.
    STATIC_REQUIRE(uniform[0] == 1.0F);
    STATIC_REQUIRE(uniform[1] == 5.0F);
    STATIC_REQUIRE(uniform[2] == 9.0F);
    STATIC_REQUIRE(uniform[3] == 13.0F);

    // And it agrees with an explicit transpose.
    STATIC_REQUIRE(uniform == m.transposed().elements());
}

TEST_CASE("a translation survives the uniform conversion", "[math][matrix]") {
    // The specific failure this conversion exists to prevent: translation lives in the
    // fourth column, and a transposed upload moves it into the fourth row, where the
    // shader's multiply ignores it.
    constexpr Mat4 t = translation(Vec2{100.0F, 200.0F});
    constexpr auto uniform = t.uniform_elements();

    // Column-major: the translation is the thirteenth through fifteenth elements.
    STATIC_REQUIRE(uniform[12] == 100.0F);
    STATIC_REQUIRE(uniform[13] == 200.0F);
    STATIC_REQUIRE(uniform[15] == 1.0F);
}

TEST_CASE("to_string prints four rows", "[math][matrix]") {
    const std::string text = Mat4::identity().to_string();
    CHECK(std::count(text.begin(), text.end(), '\n') == 4);
}
