// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/math/vector.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using atlas::math::dot;
using atlas::math::length;
using atlas::math::normalise;
using atlas::math::Rect;
using atlas::math::Vec2;
using atlas::math::Vec3;
using atlas::math::Vec4;
using Catch::Approx;

TEST_CASE("vectors default to zero", "[math][vector]") {
    STATIC_REQUIRE(Vec2{} == Vec2{0.0F, 0.0F});
    STATIC_REQUIRE(Vec3{} == Vec3{0.0F, 0.0F, 0.0F});
    STATIC_REQUIRE(Vec4{} == Vec4{0.0F, 0.0F, 0.0F, 0.0F});
}

TEST_CASE("vector arithmetic", "[math][vector]") {
    constexpr Vec2 a{3.0F, 4.0F};
    constexpr Vec2 b{1.0F, 2.0F};

    STATIC_REQUIRE(a + b == Vec2{4.0F, 6.0F});
    STATIC_REQUIRE(a - b == Vec2{2.0F, 2.0F});
    STATIC_REQUIRE(a * 2.0F == Vec2{6.0F, 8.0F});
    STATIC_REQUIRE(2.0F * a == Vec2{6.0F, 8.0F});
    STATIC_REQUIRE(a / 2.0F == Vec2{1.5F, 2.0F});
    STATIC_REQUIRE(-a == Vec2{-3.0F, -4.0F});
}

TEST_CASE("compound assignment matches the binary operators", "[math][vector]") {
    Vec2 v{1.0F, 2.0F};
    v += Vec2{3.0F, 4.0F};
    CHECK(v == Vec2{4.0F, 6.0F});
    v -= Vec2{1.0F, 1.0F};
    CHECK(v == Vec2{3.0F, 5.0F});
    v *= 2.0F;
    CHECK(v == Vec2{6.0F, 10.0F});
}

TEST_CASE("dot product and length against hand-computed values", "[math][vector]") {
    // The three-four-five triangle, so the expected answers are exact rather than whatever
    // the implementation happens to produce.
    constexpr Vec2 v{3.0F, 4.0F};
    STATIC_REQUIRE(dot(v, v) == 25.0F);
    CHECK(length(v) == Approx(5.0F));

    STATIC_REQUIRE(dot(Vec2{1.0F, 0.0F}, Vec2{0.0F, 1.0F}) == 0.0F);
    STATIC_REQUIRE(dot(Vec3{1.0F, 2.0F, 3.0F}, Vec3{4.0F, 5.0F, 6.0F}) == 32.0F);
    STATIC_REQUIRE(dot(Vec4{1.0F, 0.0F, 0.0F, 2.0F}, Vec4{3.0F, 9.0F, 9.0F, 4.0F}) == 11.0F);
}

TEST_CASE("normalise produces a unit vector", "[math][vector]") {
    const Vec2 unit = normalise(Vec2{3.0F, 4.0F});
    CHECK(unit.x == Approx(0.6F));
    CHECK(unit.y == Approx(0.8F));
    CHECK(length(unit) == Approx(1.0F));
}

TEST_CASE("normalising nothing yields nothing rather than a division by zero", "[math][vector]") {
    // A zero vector has no direction to preserve. Returning zero is defined behaviour;
    // dividing by the length would not be.
    CHECK(normalise(Vec2{}) == Vec2{});
}

TEST_CASE("Vec4 indexes in order", "[math][vector]") {
    constexpr Vec4 v{1.0F, 2.0F, 3.0F, 4.0F};
    STATIC_REQUIRE(v[0] == 1.0F);
    STATIC_REQUIRE(v[1] == 2.0F);
    STATIC_REQUIRE(v[2] == 3.0F);
    STATIC_REQUIRE(v[3] == 4.0F);
}

TEST_CASE("a rectangle reports its edges", "[math][rect]") {
    constexpr Rect rect{.position = {10.0F, 20.0F}, .size = {30.0F, 40.0F}};

    STATIC_REQUIRE(rect.left() == 10.0F);
    STATIC_REQUIRE(rect.top() == 20.0F);
    STATIC_REQUIRE(rect.right() == 40.0F);
    STATIC_REQUIRE(rect.bottom() == 60.0F);
    STATIC_REQUIRE(rect.centre() == Vec2{25.0F, 40.0F});
}

TEST_CASE("containment is half-open", "[math][rect]") {
    // Half-open, so that adjacent rectangles tile without a point belonging to both. A
    // closed test would double-count every shared edge in a grid.
    constexpr Rect rect{.position = {0.0F, 0.0F}, .size = {10.0F, 10.0F}};

    STATIC_REQUIRE(rect.contains(Vec2{0.0F, 0.0F}));
    STATIC_REQUIRE(rect.contains(Vec2{9.999F, 9.999F}));
    STATIC_REQUIRE_FALSE(rect.contains(Vec2{10.0F, 5.0F}));
    STATIC_REQUIRE_FALSE(rect.contains(Vec2{5.0F, 10.0F}));
    STATIC_REQUIRE_FALSE(rect.contains(Vec2{-0.001F, 5.0F}));
}

TEST_CASE("overlap excludes touching edges", "[math][rect]") {
    constexpr Rect a{.position = {0.0F, 0.0F}, .size = {10.0F, 10.0F}};

    STATIC_REQUIRE(a.overlaps(Rect{.position = {5.0F, 5.0F}, .size = {10.0F, 10.0F}}));
    STATIC_REQUIRE(a.overlaps(a));

    // Sharing an edge is not sharing an area, which is what culling wants: a rectangle just
    // outside the view should be culled, not drawn.
    STATIC_REQUIRE_FALSE(a.overlaps(Rect{.position = {10.0F, 0.0F}, .size = {10.0F, 10.0F}}));
    STATIC_REQUIRE_FALSE(a.overlaps(Rect{.position = {0.0F, 10.0F}, .size = {10.0F, 10.0F}}));
    STATIC_REQUIRE_FALSE(a.overlaps(Rect{.position = {20.0F, 20.0F}, .size = {1.0F, 1.0F}}));
}
