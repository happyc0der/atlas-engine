// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/platform/platform.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility>

using atlas::ErrorCode;
using atlas::platform::Platform;

TEST_CASE("a headless platform starts and stops cleanly", "[platform][lifetime]") {
    // No display is opened. This is the configuration continuous integration runs in, and
    // the one a dedicated server would use.
    auto platform = Platform::create({.video = false});

    REQUIRE(platform.has_value());
    CHECK_FALSE(platform->has_video());
    CHECK(platform->video_driver().empty());
    CHECK_FALSE(platform->quit_requested());
}

TEST_CASE("a headless platform refuses to create a window, and says why", "[platform][lifetime]") {
    auto platform = Platform::create({.video = false});
    REQUIRE(platform.has_value());

    const auto window = platform->create_window({});

    // A stub window that silently did nothing would turn a configuration mistake into a
    // mystery somewhere else entirely.
    REQUIRE_FALSE(window.has_value());
    CHECK(window.error().code() == ErrorCode::DisplayUnavailable);
    CHECK(window.error().message().contains("video"));
}

TEST_CASE("pumping a headless platform is safe and yields nothing", "[platform][lifetime]") {
    auto platform = Platform::create({.video = false});
    REQUIRE(platform.has_value());

    const auto events = platform->pump();
    CHECK(events.empty());
}

TEST_CASE("a platform can be moved", "[platform][lifetime]") {
    // Result<Platform> requires it, and a double SDL_Quit from a careless move would be a
    // crash at shutdown rather than at the mistake.
    auto created = Platform::create({.video = false});
    REQUIRE(created.has_value());

    Platform moved = std::move(*created);
    CHECK_FALSE(moved.has_video());

    const auto events = moved.pump();
    CHECK(events.empty());
}

TEST_CASE("a platform can be created and destroyed repeatedly", "[platform][lifetime]") {
    // SDL is a global library with reference-counted subsystems. Getting init and quit out
    // of balance shows up as a failure on the second run, not the first.
    for (int i = 0; i < 3; ++i) {
        const auto platform = Platform::create({.video = false});
        INFO("iteration " << i);
        REQUIRE(platform.has_value());
    }
}
