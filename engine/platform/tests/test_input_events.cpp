// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/platform/platform.hpp>

#include "sdl_keymap.hpp"
#include <SDL3/SDL_events.h>
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <span>
#include <utility>
#include <variant>

using atlas::platform::Event;
using atlas::platform::Key;
using atlas::platform::KeyPressed;
using atlas::platform::KeyReleased;
using atlas::platform::MouseButton;
using atlas::platform::MouseButtonPressed;
using atlas::platform::MouseMoved;
using atlas::platform::MouseWheel;
using atlas::platform::Platform;
using atlas::platform::QuitRequested;
using atlas::platform::WindowMinimized;
using atlas::platform::WindowRestored;

// Input state is written only by Platform, so testing it means driving a real Platform.
// These tests inject events into SDL's own queue and then pump, which exercises the
// translation and the state machine together, through the public API, on the real code
// path. Injection needs SDL, which is why this test target links it; a test is not a public
// header, so the boundary rule does not apply.
//
// This also covers the window state transitions that the dummy driver never produces on its
// own: a minimise event from a real window server looks exactly like this one.

namespace {

[[nodiscard]] std::optional<Platform> headless_platform() {
    auto platform = Platform::create({.video = false});
    if (!platform) {
        return std::nullopt;
    }
    return std::move(*platform);
}

void push_key(Key key, bool down, bool repeat = false, SDL_Keymod mod = SDL_KMOD_NONE) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = atlas::platform::detail::to_sdl_scancode(key);
    event.key.down = down;
    event.key.repeat = repeat;
    event.key.mod = mod;
    REQUIRE(SDL_PushEvent(&event));
}

void push_mouse_button(MouseButton button, bool down, float x = 0.0F, float y = 0.0F) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    switch (button) {
    case MouseButton::Left: event.button.button = SDL_BUTTON_LEFT; break;
    case MouseButton::Middle: event.button.button = SDL_BUTTON_MIDDLE; break;
    case MouseButton::Right: event.button.button = SDL_BUTTON_RIGHT; break;
    case MouseButton::X1: event.button.button = SDL_BUTTON_X1; break;
    case MouseButton::X2: event.button.button = SDL_BUTTON_X2; break;
    case MouseButton::Count: FAIL("Count is not a button"); break;
    }
    event.button.down = down;
    event.button.x = x;
    event.button.y = y;
    event.button.clicks = 1;
    REQUIRE(SDL_PushEvent(&event));
}

/// Count events of one alternative in a pumped batch.
template <typename T> [[nodiscard]] std::size_t count_of(std::span<const Event> events) {
    std::size_t count = 0;
    for (const auto& event : events) {
        if (std::holds_alternative<T>(event)) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST_CASE("a key press reaches both the event stream and the input state",
          "[platform][input][display]") {
    auto platform = headless_platform();
    REQUIRE(platform.has_value());

    push_key(Key::Space, true);
    const auto events = platform->pump();

    REQUIRE(count_of<KeyPressed>(events) == 1);
    CHECK(platform->input().is_down(Key::Space));
    CHECK(platform->input().was_pressed(Key::Space));
    CHECK_FALSE(platform->input().was_released(Key::Space));
}

TEST_CASE("held state survives a pump but the edge does not", "[platform][input][display]") {
    auto platform = headless_platform();
    REQUIRE(platform.has_value());

    push_key(Key::W, true);
    (void)platform->pump();
    REQUIRE(platform->input().was_pressed(Key::W));

    // A pump with nothing in the queue: the key is still held, but it is no longer a fresh
    // press. Getting this wrong makes a held key fire every frame.
    (void)platform->pump();

    CHECK(platform->input().is_down(Key::W));
    CHECK_FALSE(platform->input().was_pressed(Key::W));
}

TEST_CASE("an operating-system key repeat is not a new press", "[platform][input][display]") {
    auto platform = headless_platform();
    REQUIRE(platform.has_value());

    push_key(Key::A, true);
    (void)platform->pump();
    (void)platform->pump();

    push_key(Key::A, true, /*repeat=*/true);
    const auto events = platform->pump();

    // The event still arrives, flagged as a repeat, because text entry wants it.
    REQUIRE(count_of<KeyPressed>(events) == 1);
    for (const auto& event : events) {
        if (const auto* pressed = std::get_if<KeyPressed>(&event)) {
            CHECK(pressed->repeat);
        }
    }
    // The input state does not call it a fresh press, because a jump button does not.
    CHECK(platform->input().is_down(Key::A));
    CHECK_FALSE(platform->input().was_pressed(Key::A));
}

TEST_CASE("a release clears the held state and reports an edge", "[platform][input][display]") {
    auto platform = headless_platform();
    REQUIRE(platform.has_value());

    push_key(Key::Escape, true);
    (void)platform->pump();

    push_key(Key::Escape, false);
    const auto events = platform->pump();

    CHECK(count_of<KeyReleased>(events) == 1);
    CHECK_FALSE(platform->input().is_down(Key::Escape));
    CHECK(platform->input().was_released(Key::Escape));
}

TEST_CASE("modifiers are reported with the event", "[platform][input][display]") {
    auto platform = headless_platform();
    REQUIRE(platform.has_value());

    push_key(Key::S, true, false, SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT);
    const auto events = platform->pump();

    REQUIRE_FALSE(events.empty());
    for (const auto& event : events) {
        if (const auto* pressed = std::get_if<KeyPressed>(&event)) {
            CHECK(pressed->modifiers.control);
            CHECK(pressed->modifiers.shift);
            CHECK_FALSE(pressed->modifiers.alt);
        }
    }
    CHECK(platform->input().modifiers().control);
}

TEST_CASE("mouse buttons register like keys", "[platform][input][display]") {
    auto platform = headless_platform();
    REQUIRE(platform.has_value());

    push_mouse_button(MouseButton::Right, true, 120.0F, 80.0F);
    const auto events = platform->pump();

    REQUIRE(count_of<MouseButtonPressed>(events) == 1);
    CHECK(platform->input().is_down(MouseButton::Right));
    CHECK(platform->input().was_pressed(MouseButton::Right));

    push_mouse_button(MouseButton::Right, false);
    (void)platform->pump();
    CHECK_FALSE(platform->input().is_down(MouseButton::Right));
    CHECK(platform->input().was_released(MouseButton::Right));
}

TEST_CASE("mouse motion accumulates within a pump and resets between them",
          "[platform][input][display]") {
    auto platform = headless_platform();
    REQUIRE(platform.has_value());

    for (int i = 0; i < 3; ++i) {
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.x = static_cast<float>(10 * (i + 1));
        event.motion.y = 5.0F;
        event.motion.xrel = 10.0F;
        event.motion.yrel = 0.0F;
        REQUIRE(SDL_PushEvent(&event));
    }

    const auto events = platform->pump();

    CHECK(count_of<MouseMoved>(events) == 3);
    // Three motions of ten each in one frame is a movement of thirty, not of ten.
    CHECK(platform->input().mouse_delta_x() == 30.0F);
    CHECK(platform->input().mouse_position().x == 30.0F);

    (void)platform->pump();
    CHECK(platform->input().mouse_delta_x() == 0.0F);
    // Position persists; only the delta is per-frame.
    CHECK(platform->input().mouse_position().x == 30.0F);
}

TEST_CASE("wheel movement accumulates within a pump", "[platform][input][display]") {
    auto platform = headless_platform();
    REQUIRE(platform.has_value());

    for (int i = 0; i < 2; ++i) {
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_WHEEL;
        event.wheel.x = 0.0F;
        event.wheel.y = 1.5F;
        REQUIRE(SDL_PushEvent(&event));
    }

    const auto events = platform->pump();

    CHECK(count_of<MouseWheel>(events) == 2);
    CHECK(platform->input().wheel_delta_y() == 3.0F);
}

TEST_CASE("a quit request is reported once and remembered", "[platform][input][display]") {
    auto platform = headless_platform();
    REQUIRE(platform.has_value());
    REQUIRE_FALSE(platform->quit_requested());

    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    REQUIRE(SDL_PushEvent(&event));

    const auto events = platform->pump();

    CHECK(count_of<QuitRequested>(events) == 1);
    CHECK(platform->quit_requested());

    // The flag is sticky: a loop that checks it after doing other work must still see it.
    (void)platform->pump();
    CHECK(platform->quit_requested());
}

TEST_CASE("minimise and restore are translated", "[platform][input][display]") {
    // The dummy driver never produces these on its own, and there is no display here to
    // produce them either. Injecting them covers the translation, which is the part Atlas
    // is responsible for; whether a real window server sends them is SDL's responsibility.
    auto platform = headless_platform();
    REQUIRE(platform.has_value());

    SDL_Event minimized{};
    minimized.type = SDL_EVENT_WINDOW_MINIMIZED;
    minimized.window.windowID = 42;
    REQUIRE(SDL_PushEvent(&minimized));

    SDL_Event restored{};
    restored.type = SDL_EVENT_WINDOW_RESTORED;
    restored.window.windowID = 42;
    REQUIRE(SDL_PushEvent(&restored));

    const auto events = platform->pump();

    REQUIRE(count_of<WindowMinimized>(events) == 1);
    REQUIRE(count_of<WindowRestored>(events) == 1);
    for (const auto& event : events) {
        if (const auto* minimised = std::get_if<WindowMinimized>(&event)) {
            CHECK(minimised->window == 42);
        }
    }
}

TEST_CASE("a scancode Atlas does not model produces no event", "[platform][input][display]") {
    auto platform = headless_platform();
    REQUIRE(platform.has_value());

    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.scancode = SDL_SCANCODE_SCROLLLOCK;
    event.key.down = true;
    REQUIRE(SDL_PushEvent(&event));

    const auto events = platform->pump();

    // Silently ignored rather than reported as Key::Unknown, which every handler would then
    // have to filter out.
    CHECK(count_of<KeyPressed>(events) == 0);
}

TEST_CASE("the event buffer is reused across pumps", "[platform][input][display]") {
    // The span returned by pump() is valid only until the next call, and the storage is
    // reused so that a steady-state frame does not allocate. This checks the contract holds
    // rather than the performance claim, which belongs in a benchmark.
    auto platform = headless_platform();
    REQUIRE(platform.has_value());

    push_key(Key::A, true);
    const auto first = platform->pump();
    REQUIRE(first.size() == 1);

    const auto second = platform->pump();
    CHECK(second.empty());
}
