// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/math/camera.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using atlas::math::OrthoCamera;
using atlas::math::Rect;
using atlas::math::Vec2;
using atlas::math::Vec4;
using Catch::Approx;

namespace {

[[nodiscard]] OrthoCamera make_camera(float width = 800.0F, float height = 600.0F) {
    OrthoCamera camera;
    camera.set_viewport(width, height);
    return camera;
}

}  // namespace

TEST_CASE("a new camera sits at the origin at unit zoom", "[math][camera]") {
    const OrthoCamera camera;
    CHECK(camera.centre() == Vec2{});
    CHECK(camera.zoom() == 1.0F);
}

TEST_CASE("the centre of the screen is the centre of the camera", "[math][camera]") {
    auto camera = make_camera();
    camera.set_centre(Vec2{100.0F, 50.0F});

    const Vec2 world = camera.screen_to_world(Vec2{400.0F, 300.0F});
    CHECK(world.x == Approx(100.0F));
    CHECK(world.y == Approx(50.0F));
}

TEST_CASE("screen and world conversions are inverses", "[math][camera]") {
    auto camera = make_camera();
    camera.set_centre(Vec2{-37.5F, 12.25F});
    camera.set_zoom(2.5F);

    for (const Vec2 screen : {Vec2{0.0F, 0.0F}, Vec2{800.0F, 600.0F}, Vec2{123.0F, 456.0F}}) {
        const Vec2 round_trip = camera.world_to_screen(camera.screen_to_world(screen));
        INFO("screen point " << screen.x << ", " << screen.y);
        CHECK(round_trip.x == Approx(screen.x));
        CHECK(round_trip.y == Approx(screen.y));
    }
}

TEST_CASE("y increases downwards, as it does on screen", "[math][camera]") {
    // A map, a user interface and every image format already put the origin at the top left.
    // Flipping once here is cheaper than flipping at every use.
    const auto camera = make_camera();

    const Vec2 above = camera.screen_to_world(Vec2{400.0F, 100.0F});
    const Vec2 below = camera.screen_to_world(Vec2{400.0F, 500.0F});

    CHECK(above.y < below.y);
}

TEST_CASE("zoom scales the visible area", "[math][camera]") {
    auto camera = make_camera(800.0F, 600.0F);

    const Rect at_one = camera.visible_bounds();
    CHECK(at_one.size.x == Approx(800.0F));
    CHECK(at_one.size.y == Approx(600.0F));

    // Zooming in shows less of the world, not more.
    camera.set_zoom(2.0F);
    const Rect at_two = camera.visible_bounds();
    CHECK(at_two.size.x == Approx(400.0F));
    CHECK(at_two.size.y == Approx(300.0F));
}

TEST_CASE("panning moves the centre", "[math][camera]") {
    auto camera = make_camera();
    camera.pan(Vec2{10.0F, -5.0F});
    camera.pan(Vec2{5.0F, 5.0F});

    CHECK(camera.centre().x == Approx(15.0F));
    CHECK(camera.centre().y == Approx(0.0F).margin(1e-5));
}

TEST_CASE("zooming about a point keeps that point under the pointer", "[math][camera]") {
    // The property that makes wheel zoom feel right. Without it the world slides out from
    // under the pointer and the user has to chase what they were looking at.
    auto camera = make_camera();
    camera.set_centre(Vec2{100.0F, 100.0F});

    constexpr Vec2 anchor{600.0F, 200.0F};
    const Vec2 world_before = camera.screen_to_world(anchor);

    camera.zoom_about(2.0F, anchor);

    const Vec2 world_after = camera.screen_to_world(anchor);
    CHECK(world_after.x == Approx(world_before.x));
    CHECK(world_after.y == Approx(world_before.y));
    CHECK(camera.zoom() == Approx(2.0F));
}

TEST_CASE("zooming about the centre does not move the centre", "[math][camera]") {
    auto camera = make_camera();
    camera.set_centre(Vec2{50.0F, 50.0F});

    camera.zoom_about(3.0F, Vec2{400.0F, 300.0F});

    CHECK(camera.centre().x == Approx(50.0F));
    CHECK(camera.centre().y == Approx(50.0F));
}

TEST_CASE("nonsensical settings are ignored rather than breaking the camera", "[math][camera]") {
    auto camera = make_camera();
    const float original_zoom = camera.zoom();

    camera.set_zoom(0.0F);
    camera.set_zoom(-1.0F);
    CHECK(camera.zoom() == original_zoom);

    // A window dragged to nothing should leave the last good viewport in place rather than
    // divide by zero in every conversion afterwards.
    camera.set_viewport(0.0F, 100.0F);
    camera.set_viewport(100.0F, -5.0F);
    CHECK(camera.visible_bounds().size.x == Approx(800.0F));
}

TEST_CASE("the view projection maps the visible corners onto clip space", "[math][camera]") {
    auto camera = make_camera(800.0F, 600.0F);
    camera.set_centre(Vec2{1000.0F, 2000.0F});
    camera.set_zoom(2.0F);

    const Rect bounds = camera.visible_bounds();
    const auto& projection = camera.view_projection();

    // The top-left of the visible world must land at the top-left of clip space, which in
    // this convention is x = -1, y = +1.
    const Vec4 top_left = projection * Vec4{bounds.left(), bounds.top(), 0.0F, 1.0F};
    CHECK(top_left.x == Approx(-1.0F));
    CHECK(top_left.y == Approx(1.0F));

    const Vec4 bottom_right = projection * Vec4{bounds.right(), bounds.bottom(), 0.0F, 1.0F};
    CHECK(bottom_right.x == Approx(1.0F));
    CHECK(bottom_right.y == Approx(-1.0F));

    // A margin rather than Approx's default relative tolerance: comparing against exactly
    // zero relatively is meaningless, and projecting a centre two thousand units out leaves
    // about a millionth of a unit of floating-point residue.
    const Vec4 middle = projection * Vec4{camera.centre().x, camera.centre().y, 0.0F, 1.0F};
    CHECK(middle.x == Approx(0.0F).margin(1e-4));
    CHECK(middle.y == Approx(0.0F).margin(1e-4));
}

TEST_CASE("the view projection is recomputed when the camera changes", "[math][camera]") {
    auto camera = make_camera();
    const auto before = camera.view_projection();

    camera.set_centre(Vec2{500.0F, 0.0F});
    const auto after = camera.view_projection();

    CHECK_FALSE(before == after);
}
