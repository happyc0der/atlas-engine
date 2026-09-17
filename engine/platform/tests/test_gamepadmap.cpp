// SPDX-License-Identifier: GPL-3.0-or-later
// The gamepad mapping, in both directions. The same five checks the key mapping gets, because
// the same failure is possible: one wrong entry stops one button working and nothing says so.
#include "sdl_gamepadmap.hpp"
#include <catch2/catch_test_macros.hpp>

#include <set>

using atlas::platform::GamepadAxis;
using atlas::platform::GamepadButton;
using atlas::platform::detail::from_sdl_axis;
using atlas::platform::detail::from_sdl_button;
using atlas::platform::detail::to_sdl_axis;
using atlas::platform::detail::to_sdl_button;

TEST_CASE("every gamepad button maps", "[platform][gamepadmap]") {
    for (std::size_t i = 0; i < static_cast<std::size_t>(GamepadButton::Count); ++i) {
        const auto button = static_cast<GamepadButton>(i);
        INFO("button " << to_string(button));
        CHECK(to_sdl_button(button) != SDL_GAMEPAD_BUTTON_INVALID);
    }
}

TEST_CASE("every gamepad axis maps", "[platform][gamepadmap]") {
    for (std::size_t i = 0; i < static_cast<std::size_t>(GamepadAxis::Count); ++i) {
        const auto axis = static_cast<GamepadAxis>(i);
        INFO("axis " << to_string(axis));
        CHECK(to_sdl_axis(axis) != SDL_GAMEPAD_AXIS_INVALID);
    }
}

TEST_CASE("the gamepad mapping round-trips in both directions", "[platform][gamepadmap]") {
    for (std::size_t i = 0; i < static_cast<std::size_t>(GamepadButton::Count); ++i) {
        const auto button = static_cast<GamepadButton>(i);
        INFO("button " << to_string(button));
        CHECK(from_sdl_button(to_sdl_button(button)) == button);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(GamepadAxis::Count); ++i) {
        const auto axis = static_cast<GamepadAxis>(i);
        INFO("axis " << to_string(axis));
        CHECK(from_sdl_axis(to_sdl_axis(axis)) == axis);
    }
}

TEST_CASE("no two gamepad identifiers share a mapping", "[platform][gamepadmap]") {
    std::set<SDL_GamepadButton> buttons;
    for (std::size_t i = 0; i < static_cast<std::size_t>(GamepadButton::Count); ++i) {
        INFO("button " << to_string(static_cast<GamepadButton>(i)));
        CHECK(buttons.insert(to_sdl_button(static_cast<GamepadButton>(i))).second);
    }
    std::set<SDL_GamepadAxis> axes;
    for (std::size_t i = 0; i < static_cast<std::size_t>(GamepadAxis::Count); ++i) {
        INFO("axis " << to_string(static_cast<GamepadAxis>(i)));
        CHECK(axes.insert(to_sdl_axis(static_cast<GamepadAxis>(i))).second);
    }
}

TEST_CASE("the sentinels map to nothing", "[platform][gamepadmap]") {
    CHECK(to_sdl_button(GamepadButton::Count) == SDL_GAMEPAD_BUTTON_INVALID);
    CHECK(to_sdl_axis(GamepadAxis::Count) == SDL_GAMEPAD_AXIS_INVALID);
    CHECK(from_sdl_button(SDL_GAMEPAD_BUTTON_INVALID) == GamepadButton::Count);
    CHECK(from_sdl_axis(SDL_GAMEPAD_AXIS_INVALID) == GamepadAxis::Count);
}

TEST_CASE("a control Atlas does not model maps to the sentinel", "[platform][gamepadmap]") {
    // The touchpad and the extra paddles are real controls on some pads and are deliberately
    // not modelled. They must map to the sentinel rather than to a button that happens to sit
    // at the same index.
    CHECK(from_sdl_button(SDL_GAMEPAD_BUTTON_TOUCHPAD) == GamepadButton::Count);
    CHECK(from_sdl_button(SDL_GAMEPAD_BUTTON_LEFT_PADDLE1) == GamepadButton::Count);
}

TEST_CASE("a few gamepad mappings are what a reader would expect", "[platform][gamepadmap]") {
    CHECK(to_sdl_button(GamepadButton::South) == SDL_GAMEPAD_BUTTON_SOUTH);
    CHECK(to_sdl_button(GamepadButton::DpadUp) == SDL_GAMEPAD_BUTTON_DPAD_UP);
    CHECK(to_sdl_axis(GamepadAxis::LeftTrigger) == SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
}
