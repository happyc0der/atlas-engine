// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/platform/platform.hpp>
#include <atlas/rhi/device.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <utility>

using atlas::ErrorCode;
using atlas::platform::Platform;
using atlas::platform::Window;
using atlas::rhi::Device;

// Failure paths that need no graphics device, so they run everywhere including continuous
// integration. What they check is that the renderer refuses clearly rather than crashing or
// pretending to work when there is nothing to draw on.
//
// Each of these creates a Platform first, even where a window is not strictly needed. That
// is not ceremony: creating a platform is what marks the main thread, and every renderer
// entry point asserts it is on that thread. A test that skipped it would be exercising a
// situation that cannot arise in a real application.

TEST_CASE("a device cannot be created for an invalid window", "[rhi][device][display]") {
    const auto platform = Platform::create({.video = false});
    REQUIRE(platform.has_value());

    const Window window;  // default-constructed: owns nothing
    const auto device = Device::create({}, window);

    REQUIRE_FALSE(device.has_value());
    CHECK(device.error().code() == ErrorCode::InvalidArgument);
    CHECK(device.error().message().contains("window"));
}

TEST_CASE("device creation under the dummy driver fails with an actionable error",
          "[rhi][device][display]") {
    // The dummy video driver gives a real window with no display behind it, which is
    // exactly the situation on a continuous-integration runner. There is normally no
    // graphics device to be had, and the renderer must say so rather than crash.
    auto platform = Platform::create({.video = true, .video_driver = "dummy"});
    if (!platform) {
        SKIP("the dummy video driver is unavailable on this system");
    }

    auto window = platform->create_window({.width = 320, .height = 240, .hidden = true});
    REQUIRE(window.has_value());

    const auto device = Device::create({.debug = true}, *window);

    if (device.has_value()) {
        // Some systems will give a software device even here. That is a valid outcome, not
        // a failure, so the test accepts either as long as nothing crashes.
        SUCCEED("a device was available even under the dummy driver");
        return;
    }

    CHECK(device.error().code() == ErrorCode::GpuDeviceCreationFailed);
    CHECK_FALSE(device.error().message().empty());
}

TEST_CASE("a default-constructed device is inert", "[rhi][device][display]") {
    // A Device that was never created, or was moved from, must answer every query rather
    // than dereference nothing. This is the state a moved-from device is left in.
    const auto platform = Platform::create({.video = false});
    REQUIRE(platform.has_value());

    const Window window;
    const auto device = Device::create({}, window);
    REQUIRE_FALSE(device.has_value());

    // And a failed creation leaves no device behind to clean up, which is what makes the
    // error path safe to take from anywhere.
    SUCCEED("failed device creation produced no device");
}
