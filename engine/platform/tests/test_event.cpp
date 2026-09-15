// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/platform/event.hpp>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string_view>
#include <variant>

using atlas::platform::Event;
using atlas::platform::event_name;
using atlas::platform::Key;
using atlas::platform::KeyPressed;
using atlas::platform::MouseWheel;
using atlas::platform::QuitRequested;
using atlas::platform::WindowResized;

TEST_CASE("every event alternative has a distinct name", "[platform][event]") {
    // event_name uses if constexpr with a static_assert fallback, so a new alternative
    // without a name fails to compile. This checks the names are also distinct, which the
    // compiler cannot.
    std::set<std::string_view> names;

    const auto check = [&names](const Event& event) {
        const std::string_view name = event_name(event);
        INFO("alternative index " << event.index());
        CHECK_FALSE(name.empty());
        CHECK(names.insert(name).second);
    };

    check(Event{QuitRequested{}});
    check(Event{atlas::platform::WindowCloseRequested{}});
    check(Event{WindowResized{}});
    check(Event{atlas::platform::WindowMinimized{}});
    check(Event{atlas::platform::WindowRestored{}});
    check(Event{atlas::platform::WindowFocusGained{}});
    check(Event{atlas::platform::WindowFocusLost{}});
    check(Event{atlas::platform::WindowDisplayScaleChanged{}});
    check(Event{KeyPressed{}});
    check(Event{atlas::platform::KeyReleased{}});
    check(Event{atlas::platform::MouseMoved{}});
    check(Event{atlas::platform::MouseButtonPressed{}});
    check(Event{atlas::platform::MouseButtonReleased{}});
    check(Event{MouseWheel{}});

    // Every alternative was covered. If someone adds one to the variant without adding it
    // here, this fails and says so.
    CHECK(names.size() == std::variant_size_v<Event>);
}

TEST_CASE("an event carries its payload", "[platform][event]") {
    const Event event = KeyPressed{.key = Key::Escape, .modifiers = {}, .repeat = true};

    REQUIRE(std::holds_alternative<KeyPressed>(event));
    const auto& pressed = std::get<KeyPressed>(event);
    CHECK(pressed.key == Key::Escape);
    CHECK(pressed.repeat);
}

TEST_CASE("a resize event carries both the logical and the pixel size", "[platform][event]") {
    // Reporting only one is the standard high-DPI bug: a viewport sized in logical units
    // renders at a quarter of the window on a Retina display.
    const Event event = WindowResized{
        .window = 1, .size = {.width = 800, .height = 600}, .pixel_size = {1600, 1200}};

    const auto& resized = std::get<WindowResized>(event);
    CHECK(resized.size.width == 800);
    CHECK(resized.pixel_size.width == 1600);
}
