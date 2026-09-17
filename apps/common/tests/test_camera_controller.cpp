// SPDX-License-Identifier: GPL-3.0-or-later
// The camera arithmetic, without a window system.
//
// This is why the controller is split in two: only the platform may write an InputState, so a
// test cannot build one, and the part worth testing would otherwise be unreachable. The
// sampling half is a handful of field copies covered by using the applications.
#include <atlas/app/camera_controller.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using atlas::app::CameraController;
using atlas::app::CameraControls;
using atlas::app::CameraFrameInput;
using atlas::math::OrthoCamera;
using Catch::Approx;

namespace {

[[nodiscard]] OrthoCamera make_camera() {
    OrthoCamera camera;
    camera.set_viewport(800.0F, 600.0F);
    camera.set_centre({.x = 0.0F, .y = 0.0F});
    camera.set_zoom(2.0F);
    return camera;
}

}  // namespace

TEST_CASE("an untouched frame moves nothing", "[app][camera]") {
    // The controller runs every frame whether or not anything happened, so doing nothing has
    // to be free of side effects.
    OrthoCamera camera = make_camera();
    const auto before = camera.centre();
    const float zoom_before = camera.zoom();

    CameraController controller;
    controller.apply(camera, CameraFrameInput{}, 1.0F, 1.0F / 60.0F);

    CHECK(camera.centre().x == before.x);
    CHECK(camera.centre().y == before.y);
    CHECK(camera.zoom() == zoom_before);
}

TEST_CASE("dragging moves the camera opposite the pointer, scaled by zoom", "[app][camera]") {
    OrthoCamera camera = make_camera();
    CameraController controller;

    CameraFrameInput frame;
    frame.drag_started = true;
    frame.mouse_delta_x = 10.0F;
    frame.mouse_delta_y = 4.0F;
    controller.apply(camera, frame, 1.0F, 1.0F / 60.0F);

    // Opposite, so the world appears to follow the pointer, and divided by the zoom so the
    // drag covers the same distance on screen however far in the camera is.
    CHECK(camera.centre().x == Approx(-5.0F));
    CHECK(camera.centre().y == Approx(-2.0F));
}

TEST_CASE("a drag that has ended stops moving the camera", "[app][camera]") {
    OrthoCamera camera = make_camera();
    CameraController controller;

    CameraFrameInput start;
    start.drag_started = true;
    start.mouse_delta_x = 10.0F;
    controller.apply(camera, start, 1.0F, 1.0F / 60.0F);
    const auto after_drag = camera.centre();

    CameraFrameInput end;
    end.drag_ended = true;
    end.mouse_delta_x = 10.0F;
    controller.apply(camera, end, 1.0F, 1.0F / 60.0F);

    // The release is applied before the pan, so the frame the drag ends on does not move the
    // camera. That is the behaviour the three hand-written copies had, and preserving it
    // exactly is what makes this a refactor rather than a change.
    CHECK_FALSE(controller.dragging());
    CHECK(camera.centre().x == after_drag.x);

    CameraFrameInput after;
    after.mouse_delta_x = 10.0F;
    controller.apply(camera, after, 1.0F, 1.0F / 60.0F);
    CHECK(camera.centre().x == after_drag.x);
}

TEST_CASE("the display scale is applied to a drag", "[app][camera]") {
    // The M9 bug, as a test. A pointer delta is in logical units and the camera's viewport is
    // in pixels; on a two-times display, omitting the scale pans at half speed.
    OrthoCamera one = make_camera();
    OrthoCamera two = make_camera();
    CameraController first;
    CameraController second;

    CameraFrameInput frame;
    frame.drag_started = true;
    frame.mouse_delta_x = 10.0F;

    first.apply(one, frame, 1.0F, 1.0F / 60.0F);
    second.apply(two, frame, 2.0F, 1.0F / 60.0F);

    CHECK(two.centre().x == Approx(one.centre().x * 2.0F));
}

TEST_CASE("a wheel zoom leaves the world point under the pointer where it was", "[app][camera]") {
    // The invariant the M9 anchor bug broke, and which nothing checked until now. Zooming
    // about the pointer means the thing under the pointer does not move; if the anchor is
    // wrong the map slides out from under the cursor, which is exactly how the bug was
    // noticed and exactly what no test caught.
    OrthoCamera camera = make_camera();
    CameraController controller;

    const atlas::math::Vec2 pointer{.x = 620.0F, .y = 140.0F};
    const auto world_before = camera.screen_to_world(pointer);

    CameraFrameInput frame;
    frame.pointer = {.x = pointer.x, .y = pointer.y};
    frame.wheel_notches = 3.0F;
    controller.apply(camera, frame, 1.0F, 1.0F / 60.0F);

    const auto world_after = camera.screen_to_world(pointer);
    CHECK(camera.zoom() > 2.0F);
    CHECK(world_after.x == Approx(world_before.x).margin(0.001));
    CHECK(world_after.y == Approx(world_before.y).margin(0.001));
}

TEST_CASE("wheel notches in one frame compound", "[app][camera]") {
    // Summing the notches is right because the zoom is multiplicative: two notches at once
    // must equal two notches one after the other, or a fast scroll would zoom differently
    // from a slow one covering the same distance.
    OrthoCamera together = make_camera();
    OrthoCamera apart = make_camera();
    CameraController first;
    CameraController second;

    CameraFrameInput both;
    both.wheel_notches = 2.0F;
    first.apply(together, both, 1.0F, 1.0F / 60.0F);

    CameraFrameInput one;
    one.wheel_notches = 1.0F;
    second.apply(apart, one, 1.0F, 1.0F / 60.0F);
    second.apply(apart, one, 1.0F, 1.0F / 60.0F);

    CHECK(together.zoom() == Approx(apart.zoom()));
}

TEST_CASE("a stick pans by a rate over time, divided by zoom", "[app][camera]") {
    OrthoCamera camera = make_camera();
    CameraControls controls;
    controls.stick_pan_pixels_per_second = 600.0F;
    CameraController controller{controls};

    CameraFrameInput frame;
    frame.pan_x = 1.0F;
    controller.apply(camera, frame, 1.0F, 1.0F);

    // Full deflection for one second, at zoom two: 600 screen pixels is 300 world units.
    CHECK(camera.centre().x == Approx(300.0F));
}

TEST_CASE("a stick at rest after the dead zone pans nothing", "[app][camera]") {
    OrthoCamera camera = make_camera();
    CameraController controller;

    CameraFrameInput frame;
    frame.pan_x = 0.0F;
    frame.pan_y = 0.0F;
    controller.apply(camera, frame, 1.0F, 1.0F);

    CHECK(camera.centre().x == 0.0F);
    CHECK(camera.centre().y == 0.0F);
}

TEST_CASE("a stick zoom keeps the centre and compounds over time", "[app][camera]") {
    // No anchor, and none is needed: the camera's centre is the world point under the middle
    // of the viewport, which is where a gamepad has to zoom about because it has no pointer.
    OrthoCamera camera = make_camera();
    camera.set_centre({.x = 40.0F, .y = -12.0F});
    CameraController controller;

    CameraFrameInput frame;
    frame.zoom_axis = 1.0F;
    controller.apply(camera, frame, 1.0F, 0.5F);
    controller.apply(camera, frame, 1.0F, 0.5F);

    CHECK(camera.centre().x == Approx(40.0F));
    CHECK(camera.centre().y == Approx(-12.0F));
    // Two half-seconds at one unit a second: e to the power one.
    CHECK(camera.zoom() == Approx(2.0F * 2.718281828F).epsilon(0.001));
}

TEST_CASE("pulling both triggers cancels", "[app][camera]") {
    OrthoCamera camera = make_camera();
    CameraController controller;

    CameraFrameInput frame;
    frame.zoom_axis = 0.0F;  // what the sampler produces when both are fully pulled
    controller.apply(camera, frame, 1.0F, 1.0F);

    CHECK(camera.zoom() == Approx(2.0F));
}
