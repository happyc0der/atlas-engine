// SPDX-License-Identifier: GPL-3.0-or-later
// Gamepads, without a gamepad.
//
// The window system delivers button and axis events only for a pad it has opened, so a test
// cannot fake one by pushing an event with an invented identifier the way the key tests do.
// A virtual joystick is the honest substitute: it produces real connect, button and axis
// events through the ordinary path, needs no hardware and no display, and therefore runs on
// every continuous-integration machine.
#include <atlas/platform/platform.hpp>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <optional>
#include <span>
#include <utility>

using atlas::platform::Event;
using atlas::platform::GamepadAxis;
using atlas::platform::GamepadButton;
using atlas::platform::GamepadButtonPressed;
using atlas::platform::GamepadButtonReleased;
using atlas::platform::GamepadConnected;
using atlas::platform::GamepadDisconnected;
using atlas::platform::kMaxGamepads;
using atlas::platform::Platform;

namespace {

[[nodiscard]] std::optional<Platform> gamepad_platform() {
    auto platform = Platform::create({.video = false, .gamepad = true});
    if (!platform || !platform->has_gamepad_support()) {
        return std::nullopt;
    }
    return std::move(*platform);
}

template <typename T> [[nodiscard]] std::size_t count_of(std::span<const Event> events) {
    std::size_t total = 0;
    for (const auto& event : events) {
        if (std::holds_alternative<T>(event)) {
            ++total;
        }
    }
    return total;
}

/// A virtual pad, attached for the life of the object.
///
/// Declared as a gamepad rather than a bare joystick so the window system gives it a gamepad
/// mapping and reports it through the gamepad events Atlas translates.
class VirtualPad {
  public:
    VirtualPad() {
        SDL_VirtualJoystickDesc desc{};
        desc.version = sizeof(desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        m_id = SDL_AttachVirtualJoystick(&desc);
        if (m_id != 0) {
            m_joystick = SDL_OpenJoystick(m_id);
        }
    }

    ~VirtualPad() {
        if (m_joystick != nullptr) {
            SDL_CloseJoystick(m_joystick);
        }
        if (m_id != 0) {
            SDL_DetachVirtualJoystick(m_id);
        }
    }

    VirtualPad(const VirtualPad&) = delete;
    VirtualPad& operator=(const VirtualPad&) = delete;
    VirtualPad(VirtualPad&&) = delete;
    VirtualPad& operator=(VirtualPad&&) = delete;

    [[nodiscard]] bool attached() const noexcept { return m_id != 0 && m_joystick != nullptr; }

    void press(SDL_GamepadButton button, bool down) const {
        REQUIRE(SDL_SetJoystickVirtualButton(m_joystick, static_cast<int>(button), down));
    }

    void move(SDL_GamepadAxis axis, std::int16_t value) const {
        REQUIRE(SDL_SetJoystickVirtualAxis(m_joystick, static_cast<int>(axis), value));
    }

    void detach() {
        if (m_joystick != nullptr) {
            SDL_CloseJoystick(m_joystick);
            m_joystick = nullptr;
        }
        if (m_id != 0) {
            SDL_DetachVirtualJoystick(m_id);
            m_id = 0;
        }
    }

  private:
    SDL_JoystickID m_id = 0;
    SDL_Joystick* m_joystick = nullptr;
};

}  // namespace

TEST_CASE("a gamepad takes a slot and is reported", "[platform][gamepad][display]") {
    auto platform = gamepad_platform();
    if (!platform) {
        SKIP("the gamepad subsystem is unavailable on this machine");
    }
    const VirtualPad pad;
    if (!pad.attached()) {
        SKIP("virtual joysticks are unavailable in this build of the window system");
    }

    const auto events = platform->pump();
    REQUIRE(count_of<GamepadConnected>(events) == 1);
    CHECK(platform->input().is_connected(0));
    // Slots are assigned from the lowest free one, so the first pad is always slot zero.
    for (const auto& event : events) {
        if (const auto* connected = std::get_if<GamepadConnected>(&event)) {
            CHECK(connected->gamepad == 0);
        }
    }
}

TEST_CASE("a button press is reported once and its edge clears next frame",
          "[platform][gamepad][display]") {
    auto platform = gamepad_platform();
    if (!platform) {
        SKIP("the gamepad subsystem is unavailable on this machine");
    }
    const VirtualPad pad;
    if (!pad.attached()) {
        SKIP("virtual joysticks are unavailable in this build of the window system");
    }
    (void)platform->pump();

    pad.press(SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_UpdateGamepads();
    const auto pressed = platform->pump();

    CHECK(count_of<GamepadButtonPressed>(pressed) == 1);
    CHECK(platform->input().is_down(0, GamepadButton::South));
    CHECK(platform->input().was_pressed(0, GamepadButton::South));

    // Held state persists; the edge does not. A caller polling was_pressed every frame would
    // otherwise act on one press forever.
    (void)platform->pump();
    CHECK(platform->input().is_down(0, GamepadButton::South));
    CHECK_FALSE(platform->input().was_pressed(0, GamepadButton::South));

    pad.press(SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_UpdateGamepads();
    const auto released = platform->pump();
    CHECK(count_of<GamepadButtonReleased>(released) == 1);
    CHECK_FALSE(platform->input().is_down(0, GamepadButton::South));
    CHECK(platform->input().was_released(0, GamepadButton::South));
}

TEST_CASE("an axis keeps its value across frames", "[platform][gamepad][display]") {
    // The reason there is no axis event: a stick that is held is still deflected, so its
    // position is state. A caller reads it every frame and must keep reading the same value.
    auto platform = gamepad_platform();
    if (!platform) {
        SKIP("the gamepad subsystem is unavailable on this machine");
    }
    const VirtualPad pad;
    if (!pad.attached()) {
        SKIP("virtual joysticks are unavailable in this build of the window system");
    }
    (void)platform->pump();

    pad.move(SDL_GAMEPAD_AXIS_LEFTX, 32767);
    SDL_UpdateGamepads();
    (void)platform->pump();

    const float first = platform->input().axis(0, GamepadAxis::LeftX);
    CHECK(first > 0.99F);

    (void)platform->pump();
    (void)platform->pump();
    CHECK(platform->input().axis(0, GamepadAxis::LeftX) == first);
}

TEST_CASE("the dead zone silences a resting stick and full travel still reaches one",
          "[platform][gamepad][display]") {
    auto platform = gamepad_platform();
    if (!platform) {
        SKIP("the gamepad subsystem is unavailable on this machine");
    }
    const VirtualPad pad;
    if (!pad.attached()) {
        SKIP("virtual joysticks are unavailable in this build of the window system");
    }
    (void)platform->pump();

    // A stick that has not quite returned to centre. Without the dead zone this drifts the
    // camera for as long as the application runs.
    pad.move(SDL_GAMEPAD_AXIS_LEFTX, 3000);
    SDL_UpdateGamepads();
    (void)platform->pump();
    CHECK(platform->input().axis(0, GamepadAxis::LeftX) == 0.0F);

    // Full travel reads exactly one, not one minus the dead zone: the remainder is rescaled.
    // The negative extreme is one step further from centre than the positive, and must still
    // read exactly minus one rather than a shade beyond it.
    pad.move(SDL_GAMEPAD_AXIS_LEFTX, -32768);
    SDL_UpdateGamepads();
    (void)platform->pump();
    CHECK(platform->input().axis(0, GamepadAxis::LeftX) == -1.0F);
}

TEST_CASE("disconnecting frees the slot and releases what the pad was holding",
          "[platform][gamepad][display]") {
    auto platform = gamepad_platform();
    if (!platform) {
        SKIP("the gamepad subsystem is unavailable on this machine");
    }
    VirtualPad pad;
    if (!pad.attached()) {
        SKIP("virtual joysticks are unavailable in this build of the window system");
    }
    (void)platform->pump();

    pad.press(SDL_GAMEPAD_BUTTON_SOUTH, true);
    pad.move(SDL_GAMEPAD_AXIS_LEFTX, 32767);
    SDL_UpdateGamepads();
    (void)platform->pump();
    REQUIRE(platform->input().is_down(0, GamepadButton::South));

    pad.detach();
    const auto events = platform->pump();

    CHECK(count_of<GamepadDisconnected>(events) == 1);
    CHECK_FALSE(platform->input().is_connected(0));
    // The point of clearing on disconnect: a stick held at the moment the cable is pulled
    // would otherwise pan the camera forever.
    CHECK(platform->input().axis(0, GamepadAxis::LeftX) == 0.0F);
    CHECK_FALSE(platform->input().is_down(0, GamepadButton::South));
}

TEST_CASE("an empty or out-of-range slot answers rather than asserting",
          "[platform][gamepad][display]") {
    // A caller polling four slots every frame should not have to check each one first.
    auto platform = gamepad_platform();
    if (!platform) {
        SKIP("the gamepad subsystem is unavailable on this machine");
    }

    CHECK_FALSE(platform->input().is_connected(0));
    CHECK_FALSE(platform->input().is_down(0, GamepadButton::South));
    CHECK(platform->input().axis(0, GamepadAxis::LeftX) == 0.0F);

    const auto beyond = static_cast<atlas::platform::GamepadId>(kMaxGamepads + 3);
    CHECK_FALSE(platform->input().is_connected(beyond));
    CHECK_FALSE(platform->input().was_pressed(beyond, GamepadButton::Start));
    CHECK(platform->input().axis(beyond, GamepadAxis::RightY) == 0.0F);
}

TEST_CASE("a reused slot does not inherit the previous pad's state",
          "[platform][gamepad][display]") {
    // The scenario that makes clearing on disconnect matter. While a slot is empty its
    // queries answer false and zero anyway, so a stale stick is invisible; it becomes visible
    // the moment a new pad takes the slot. Unplugging a controller mid-deflection and
    // plugging in another is an ordinary thing to do, and the camera must not inherit the
    // first one's lean.
    auto platform = gamepad_platform();
    if (!platform) {
        SKIP("the gamepad subsystem is unavailable on this machine");
    }

    {
        const VirtualPad first;
        if (!first.attached()) {
            SKIP("virtual joysticks are unavailable in this build of the window system");
        }
        (void)platform->pump();
        first.move(SDL_GAMEPAD_AXIS_LEFTX, 32767);
        first.press(SDL_GAMEPAD_BUTTON_SOUTH, true);
        SDL_UpdateGamepads();
        (void)platform->pump();
        REQUIRE(platform->input().axis(0, GamepadAxis::LeftX) > 0.99F);
        REQUIRE(platform->input().is_down(0, GamepadButton::South));
    }
    (void)platform->pump();
    REQUIRE_FALSE(platform->input().is_connected(0));

    const VirtualPad second;
    if (!second.attached()) {
        SKIP("virtual joysticks are unavailable in this build of the window system");
    }
    (void)platform->pump();

    // The lowest free slot is the one just vacated, so this is the same slot as before.
    REQUIRE(platform->input().is_connected(0));
    CHECK(platform->input().axis(0, GamepadAxis::LeftX) == 0.0F);
    CHECK_FALSE(platform->input().is_down(0, GamepadButton::South));
}
