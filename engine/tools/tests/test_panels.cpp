// SPDX-License-Identifier: GPL-3.0-or-later
// The decisions the panels make, tested without a graphics device. The panels themselves need
// one; what they decide does not, and this is the half that can be wrong invisibly: a filter
// that hides errors looks exactly like a system with nothing to report.
#include <atlas/tools/panels.hpp>
#include <atlas/tools/text_keys.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <set>
#include <thread>

using atlas::log::LogBuffer;
using atlas::log::Severity;
using atlas::tools::LogFilter;
using atlas::tools::matches;
using atlas::tools::SimulationControlsRequest;
using atlas::tools::speed_name;

namespace {

[[nodiscard]] LogBuffer::Entry entry(Severity severity, std::string category, std::string message) {
    return LogBuffer::Entry{
        .timestamp = std::chrono::system_clock::now(),
        .thread = std::this_thread::get_id(),
        .category = std::move(category),
        .severity = severity,
        .message = std::move(message),
    };
}

}  // namespace

TEST_CASE("the severity floor hides what is below it and nothing above", "[tools][panels]") {
    const LogFilter filter{.min_severity = Severity::Warning};

    CHECK_FALSE(matches(entry(Severity::Trace, "app", "x"), filter));
    CHECK_FALSE(matches(entry(Severity::Debug, "app", "x"), filter));
    CHECK_FALSE(matches(entry(Severity::Info, "app", "x"), filter));
    // The floor is inclusive: a filter set to warning must show warnings, or an engineer who
    // set it to see problems would see everything except the first one.
    CHECK(matches(entry(Severity::Warning, "app", "x"), filter));
    CHECK(matches(entry(Severity::Error, "app", "x"), filter));
    CHECK(matches(entry(Severity::Fatal, "app", "x"), filter));
}

TEST_CASE("an empty category filter matches everything", "[tools][panels]") {
    // The default. A filter that matched nothing until something was typed would look like a
    // broken console.
    const LogFilter filter{};
    CHECK(matches(entry(Severity::Trace, "app", "x"), filter));
    CHECK(matches(entry(Severity::Trace, "", "x"), filter));
}

TEST_CASE("the category filter is a substring, not an equality", "[tools][panels]") {
    LogFilter filter{.min_severity = Severity::Trace, .category_substring = "ass"};
    CHECK(matches(entry(Severity::Info, "assets", "x"), filter));
    CHECK_FALSE(matches(entry(Severity::Info, "app", "x"), filter));

    filter.category_substring = "assets";
    CHECK(matches(entry(Severity::Info, "assets", "x"), filter));

    // Not applied to the message: an engineer filtering by category and seeing a match because
    // the word appeared in someone's error text would be worse than no filter.
    filter.category_substring = "needle";
    CHECK_FALSE(matches(entry(Severity::Info, "app", "a needle in the message"), filter));
}

TEST_CASE("both filters must pass, not either", "[tools][panels]") {
    const LogFilter filter{.min_severity = Severity::Error, .category_substring = "rhi"};
    CHECK(matches(entry(Severity::Error, "rhi", "x"), filter));
    CHECK_FALSE(matches(entry(Severity::Info, "rhi", "x"), filter));
    CHECK_FALSE(matches(entry(Severity::Error, "app", "x"), filter));
}

TEST_CASE("a controls request with nothing set is empty", "[tools][panels]") {
    // What a frame in which nobody clicked produces. The application checks this before doing
    // anything, so a panel that reported a request every frame would restart the simulation
    // sixty times a second.
    const SimulationControlsRequest none;
    CHECK(none.empty());

    SimulationControlsRequest stepped;
    stepped.single_step = true;
    CHECK_FALSE(stepped.empty());

    SimulationControlsRequest sped;
    sped.speed = atlas::sim::Speed::times(4);
    CHECK_FALSE(sped.empty());

    SimulationControlsRequest moded;
    moded.mode_index = 0;  // index zero is a request, not an absence
    CHECK_FALSE(moded.empty());

    SimulationControlsRequest viewed;
    viewed.reset_view = true;
    CHECK_FALSE(viewed.empty());

    SimulationControlsRequest saved;
    saved.save = true;
    CHECK_FALSE(saved.empty());

    SimulationControlsRequest loaded;
    loaded.load = true;
    CHECK_FALSE(loaded.empty());
}

TEST_CASE("every speed has a name", "[tools][panels]") {
    using atlas::sim::Speed;
    namespace keys = atlas::tools::keys;

    // Keys since M16, not the words themselves. What a key says is the catalogue's business;
    // what this asserts is that each speed maps to its own key and no two collide.
    CHECK(speed_name(Speed::paused()) == keys::kSpeedPaused);
    CHECK(speed_name(Speed::unbounded()) == keys::kSpeedUnbounded);
    CHECK(speed_name(Speed::normal()) == keys::kSpeed1x);
    CHECK(speed_name(Speed::times(2)) == keys::kSpeed2x);
    // 4x was missing until a mutation run pointed it out: making it report as 2x broke
    // nothing, because every other multiplier was checked and this one was not.
    CHECK(speed_name(Speed::times(4)) == keys::kSpeed4x);
    CHECK(speed_name(Speed::times(8)) == keys::kSpeed8x);
    CHECK(speed_name(Speed::times(3)) == keys::kSpeedCustom);

    // A fractional speed is realtime with a denominator, and naming it "1x" because its
    // numerator is one would be a lie on the one row that says how fast time is running. The
    // lab carried its own copy of this function without this check until M16.
    const Speed half{.policy = atlas::sim::SpeedPolicy::Realtime, .numerator = 1, .denominator = 2};
    CHECK(speed_name(half) == keys::kSpeedCustom);
}

TEST_CASE("every action has a key", "[tools][panels]") {
    // A control reachable only by gamepad would be invisible to anyone without one, and this
    // is an engineering tool that is mostly driven from a keyboard.
    for (std::size_t i = 0; i < static_cast<std::size_t>(atlas::tools::ControlAction::Count); ++i) {
        const auto action = static_cast<atlas::tools::ControlAction>(i);
        bool found = false;
        for (const auto& binding : atlas::tools::control_bindings()) {
            if (binding.action == action && binding.key != atlas::platform::Key::Unknown) {
                found = true;
            }
        }
        INFO("action index " << i);
        CHECK(found);
    }
}

TEST_CASE("no key and no button means two things", "[tools][panels]") {
    // The whole reason the key and the button sit on one row: a duplicate would make one of
    // them shadow the other, and which one would depend on the order of the table.
    std::set<atlas::platform::Key> keys;
    std::set<atlas::platform::GamepadButton> buttons;
    for (const auto& binding : atlas::tools::control_bindings()) {
        INFO("action index " << static_cast<int>(binding.action));
        CHECK(keys.insert(binding.key).second);
        if (binding.button != atlas::platform::GamepadButton::Count) {
            CHECK(buttons.insert(binding.button).second);
        }
    }
}

TEST_CASE("a key and its button ask for the same thing", "[tools][panels]") {
    // The property the table exists to guarantee. If these ever disagree, one of the two input
    // devices has quietly stopped matching the other.
    for (const auto& binding : atlas::tools::control_bindings()) {
        if (binding.button == atlas::platform::GamepadButton::Count) {
            continue;
        }
        const auto from_key = atlas::tools::action_for(binding.key);
        const auto from_button = atlas::tools::action_for(binding.button);
        REQUIRE(from_key.has_value());
        REQUIRE(from_button.has_value());
        CHECK(*from_key == *from_button);
    }
}

TEST_CASE("an unbound key or button asks for nothing", "[tools][panels]") {
    CHECK_FALSE(atlas::tools::action_for(atlas::platform::Key::Q).has_value());
    CHECK_FALSE(atlas::tools::action_for(atlas::platform::Key::Unknown).has_value());
    CHECK_FALSE(atlas::tools::action_for(atlas::platform::GamepadButton::Guide).has_value());
    // The sentinel is not a button and must not match the rows that have no button.
    CHECK_FALSE(atlas::tools::action_for(atlas::platform::GamepadButton::Count).has_value());
}

TEST_CASE("pause toggles against the present rather than setting a state", "[tools][panels]") {
    using atlas::tools::ControlAction;
    using atlas::tools::ControlsContext;
    using atlas::tools::request_for;

    const auto running = request_for(ControlAction::TogglePause,
                                     ControlsContext{.speed = atlas::sim::Speed::normal()});
    REQUIRE(running.speed.has_value());
    CHECK(running.speed->policy == atlas::sim::SpeedPolicy::Paused);

    const auto paused = request_for(ControlAction::TogglePause,
                                    ControlsContext{.speed = atlas::sim::Speed::paused()});
    REQUIRE(paused.speed.has_value());
    CHECK(paused.speed->policy == atlas::sim::SpeedPolicy::Realtime);
}

TEST_CASE("faster and slower step the ladder and stop at its ends", "[tools][panels]") {
    using atlas::sim::Speed;
    using atlas::tools::ControlAction;
    using atlas::tools::ControlsContext;
    using atlas::tools::request_for;

    const auto faster =
        request_for(ControlAction::Faster, ControlsContext{.speed = Speed::normal()});
    REQUIRE(faster.speed.has_value());
    CHECK(faster.speed->numerator == 2);

    // Off the top of the ladder is unbounded, and staying there is what a held shoulder does.
    const auto top =
        request_for(ControlAction::Faster, ControlsContext{.speed = Speed::unbounded()});
    REQUIRE(top.speed.has_value());
    CHECK(top.speed->policy == atlas::sim::SpeedPolicy::Unbounded);

    const auto slower =
        request_for(ControlAction::Slower, ControlsContext{.speed = Speed::times(4)});
    REQUIRE(slower.speed.has_value());
    CHECK(slower.speed->numerator == 2);

    const auto bottom =
        request_for(ControlAction::Slower, ControlsContext{.speed = Speed::normal()});
    REQUIRE(bottom.speed.has_value());
    CHECK(bottom.speed->numerator == 1);

    // From paused, stepping up starts at the bottom rather than jumping to wherever a search
    // for a paused speed happened to land.
    const auto from_paused =
        request_for(ControlAction::Faster, ControlsContext{.speed = Speed::paused()});
    REQUIRE(from_paused.speed.has_value());
    CHECK(from_paused.speed->numerator == 2);
}

TEST_CASE("the next mode wraps", "[tools][panels]") {
    const auto wrapped = atlas::tools::request_for(
        atlas::tools::ControlAction::NextMode,
        atlas::tools::ControlsContext{
            .speed = atlas::sim::Speed::normal(), .mode_index = 3, .mode_count = 4});
    REQUIRE(wrapped.mode_index.has_value());
    CHECK(*wrapped.mode_index == 0);
}
