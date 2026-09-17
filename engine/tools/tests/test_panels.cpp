// SPDX-License-Identifier: GPL-3.0-or-later
// The decisions the panels make, tested without a graphics device. The panels themselves need
// one; what they decide does not, and this is the half that can be wrong invisibly: a filter
// that hides errors looks exactly like a system with nothing to report.
#include <atlas/tools/panels.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
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
    CHECK(speed_name(Speed::paused()) == "paused");
    CHECK(speed_name(Speed::unbounded()) == "unbounded");
    CHECK(speed_name(Speed::normal()) == "1x");
    CHECK(speed_name(Speed::times(2)) == "2x");
    CHECK(speed_name(Speed::times(8)) == "8x");
    CHECK(speed_name(Speed::times(3)) == "custom");

    // A fractional speed is realtime with a denominator, and naming it "1x" because its
    // numerator is one would be a lie on the one row that says how fast time is running.
    const Speed half{.policy = atlas::sim::SpeedPolicy::Realtime, .numerator = 1, .denominator = 2};
    CHECK(speed_name(half) == "custom");
}
