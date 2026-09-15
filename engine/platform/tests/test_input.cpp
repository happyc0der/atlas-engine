// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/platform/input.hpp>

#include <catch2/catch_test_macros.hpp>

using atlas::platform::InputState;
using atlas::platform::Key;
using atlas::platform::MouseButton;

// InputState's mutators are private to Platform, deliberately: a caller that could forge
// input would make the split between "events say what changed" and "state says what is
// true" meaningless. So this file covers only what is observable without mutating, and the
// real behaviour, edges and repeats and releases, is tested through a live Platform with
// injected events in test_input_events.cpp. That is the better test anyway, because it
// exercises the translation and the state machine together.

TEST_CASE("a fresh input state has nothing held", "[platform][input]") {
    const InputState state;

    CHECK_FALSE(state.is_down(Key::A));
    CHECK_FALSE(state.was_pressed(Key::A));
    CHECK_FALSE(state.was_released(Key::A));
    CHECK_FALSE(state.is_down(MouseButton::Left));
    CHECK_FALSE(state.was_pressed(MouseButton::Left));

    CHECK(state.mouse_delta_x() == 0.0F);
    CHECK(state.mouse_delta_y() == 0.0F);
    CHECK(state.wheel_delta_x() == 0.0F);
    CHECK(state.wheel_delta_y() == 0.0F);
    CHECK(state.mouse_position().x == 0.0F);

    const auto modifiers = state.modifiers();
    CHECK_FALSE(modifiers.shift);
    CHECK_FALSE(modifiers.control);
    CHECK_FALSE(modifiers.alt);
    CHECK_FALSE(modifiers.super);
}

TEST_CASE("querying an out-of-range key is safe and false", "[platform][input]") {
    // The arrays are sized by Key::Count. A query with a value outside the enumeration must
    // not index past the end, however it was produced.
    const InputState state;
    const auto bogus = static_cast<Key>(9999);

    CHECK_FALSE(state.is_down(bogus));
    CHECK_FALSE(state.was_pressed(bogus));
    CHECK_FALSE(state.was_released(bogus));
}

TEST_CASE("Count is not a queryable key or button", "[platform][input]") {
    const InputState state;

    CHECK_FALSE(state.is_down(Key::Count));
    CHECK_FALSE(state.is_down(MouseButton::Count));
}
