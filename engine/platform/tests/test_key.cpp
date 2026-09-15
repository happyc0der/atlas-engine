// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/platform/key.hpp>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string_view>

using atlas::platform::Key;
using atlas::platform::MouseButton;
using atlas::platform::to_string;

TEST_CASE("every key has a distinct name", "[platform][key]") {
    // A key without a name prints as a number in a binding UI or a log. A duplicate name is
    // worse: two different keys that look identical to whoever is reading.
    std::set<std::string_view> names;
    for (std::uint16_t i = 0; i <= static_cast<std::uint16_t>(Key::Count); ++i) {
        const auto key = static_cast<Key>(i);
        const std::string_view name = to_string(key);

        INFO("key index " << i);
        CHECK_FALSE(name.empty());
        CHECK(name != "Unrecognised");
        CHECK(names.insert(name).second);
    }
}

TEST_CASE("every mouse button has a distinct name", "[platform][key]") {
    std::set<std::string_view> names;
    for (std::uint8_t i = 0; i <= static_cast<std::uint8_t>(MouseButton::Count); ++i) {
        const auto button = static_cast<MouseButton>(i);
        const std::string_view name = to_string(button);

        INFO("button index " << static_cast<int>(i));
        CHECK_FALSE(name.empty());
        CHECK(name != "Unrecognised");
        CHECK(names.insert(name).second);
    }
}

TEST_CASE("a value outside the enumeration is reported rather than crashing", "[platform][key]") {
    const auto bogus = static_cast<Key>(9999);
    CHECK(to_string(bogus) == "Unrecognised");
}

TEST_CASE("Count is the number of keys, and Unknown is first", "[platform][key]") {
    CHECK(static_cast<std::uint16_t>(Key::Unknown) == 0);
    CHECK(static_cast<std::uint16_t>(Key::A) == 1);
    CHECK(static_cast<std::uint16_t>(Key::Count) > static_cast<std::uint16_t>(Key::Slash));
}
