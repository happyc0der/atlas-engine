// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/platform/platform.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <variant>

using atlas::platform::Event;
using atlas::platform::Platform;
using atlas::platform::Window;

// These tests use SDL's "dummy" video driver: a real window and a real event pipeline with
// no display behind them. That is what lets window creation, resizing and destruction be
// exercised on a continuous-integration runner that has no display at all. What it cannot
// cover is anything a real window server decides, notably minimise and restore, which is
// stated plainly in docs/ROADMAP.md rather than implied to be tested.

namespace {

/// Create a platform on the dummy driver, or skip the test if it is unavailable.
[[nodiscard]] std::optional<Platform> dummy_platform() {
    auto platform = Platform::create({.video = true, .video_driver = "dummy"});
    if (!platform) {
        return std::nullopt;
    }
    return std::move(*platform);
}

}  // namespace

TEST_CASE("a window can be created and destroyed without a display",
          "[platform][window][display]") {
    auto platform = dummy_platform();
    if (!platform) {
        SKIP("the dummy video driver is unavailable on this system");
    }
    CHECK(platform->video_driver() == "dummy");

    {
        auto window = platform->create_window(
            {.title = "Atlas test", .width = 640, .height = 480, .hidden = true});
        REQUIRE(window.has_value());

        CHECK(window->valid());
        CHECK(window->id() != 0);
        CHECK(window->size().width == 640);
        CHECK(window->size().height == 480);
        CHECK(window->title() == "Atlas test");
    }
    // The window is destroyed here. Anything left dangling would show up under
    // AddressSanitizer, which this test runs under in the sanitizer job.
    SUCCEED("window destroyed cleanly");
}

TEST_CASE("a window reports both a logical and a pixel size", "[platform][window][display]") {
    auto platform = dummy_platform();
    if (!platform) {
        SKIP("the dummy video driver is unavailable on this system");
    }

    auto window = platform->create_window({.width = 800, .height = 600, .hidden = true});
    REQUIRE(window.has_value());

    const auto logical = window->size();
    const auto pixels = window->pixel_size();

    CHECK(logical.width == 800);
    // The dummy driver has no display scale, so the two match here. On a scaled display they
    // differ, and the renderer must use the pixel size. The distinction is what matters.
    CHECK(pixels.width > 0);
    CHECK(pixels.height > 0);
    CHECK(window->display_scale() > 0.0F);
}

TEST_CASE("a window can be resized", "[platform][window][display]") {
    auto platform = dummy_platform();
    if (!platform) {
        SKIP("the dummy video driver is unavailable on this system");
    }

    auto window = platform->create_window({.width = 640, .height = 480, .hidden = true});
    REQUIRE(window.has_value());

    REQUIRE(window->set_size({.width = 1024, .height = 768}).has_value());
    CHECK(window->size() == atlas::platform::Extent2D{.width = 1024, .height = 768});
}

TEST_CASE("resizing to nothing is rejected", "[platform][window][display]") {
    auto platform = dummy_platform();
    if (!platform) {
        SKIP("the dummy video driver is unavailable on this system");
    }

    auto window = platform->create_window({.width = 640, .height = 480, .hidden = true});
    REQUIRE(window.has_value());

    const auto status = window->set_size({.width = 0, .height = 0});
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == atlas::ErrorCode::InvalidArgument);
}

TEST_CASE("creating a window with no size is rejected", "[platform][window][display]") {
    auto platform = dummy_platform();
    if (!platform) {
        SKIP("the dummy video driver is unavailable on this system");
    }

    const auto window = platform->create_window({.width = 0, .height = 720});
    REQUIRE_FALSE(window.has_value());
    CHECK(window.error().code() == atlas::ErrorCode::InvalidArgument);
}

TEST_CASE("a window can be moved without leaking or double-destroying",
          "[platform][window][display]") {
    auto platform = dummy_platform();
    if (!platform) {
        SKIP("the dummy video driver is unavailable on this system");
    }

    auto created = platform->create_window({.width = 320, .height = 240, .hidden = true});
    REQUIRE(created.has_value());
    const auto id = created->id();

    const Window moved = std::move(*created);

    CHECK(moved.valid());
    CHECK(moved.id() == id);
    // The moved-from window must be inert: destroying it at scope exit must not destroy the
    // window the moved-to one now owns.
    CHECK_FALSE(created->valid());  // NOLINT(bugprone-use-after-move): checking that state
}

TEST_CASE("the title can be changed", "[platform][window][display]") {
    auto platform = dummy_platform();
    if (!platform) {
        SKIP("the dummy video driver is unavailable on this system");
    }

    auto window = platform->create_window({.title = "before", .hidden = true});
    REQUIRE(window.has_value());
    CHECK(window->title() == "before");

    window->set_title("after");
    CHECK(window->title() == "after");
}

TEST_CASE("an invalid window is inert rather than dangerous", "[platform][window][display]") {
    // A default-constructed window owns nothing. Every accessor must cope, because a
    // moved-from window is in exactly this state.
    Window window;

    CHECK_FALSE(window.valid());
    CHECK(window.id() == 0);
    CHECK(window.size() == atlas::platform::Extent2D{});
    CHECK(window.pixel_size() == atlas::platform::Extent2D{});
    CHECK(window.display_scale() == 1.0F);
    CHECK_FALSE(window.is_minimized());
    CHECK_FALSE(window.has_focus());
    CHECK(window.is_hidden());

    window.set_title("ignored");
    window.show();
    window.hide();

    const auto status = window.set_size({.width = 100, .height = 100});
    CHECK_FALSE(status.has_value());
}
