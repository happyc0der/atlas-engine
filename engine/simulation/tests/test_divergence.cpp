// SPDX-License-Identifier: GPL-3.0-or-later
// Attributing a divergence to a system.
//
// Driven directly with two lists of hashes rather than through a replay, which is the whole
// point of the extraction: lockstep compares one peer's hashes against another's, and neither
// side is a recording.
#include <atlas/simulation/divergence.hpp>

#include "synthetic_systems.hpp"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

using atlas::sim::attribute_divergence;
using atlas::sim::system_id;
using atlas::sim::SystemHash;
using atlas::sim::SystemId;
using atlas::sim::testing::Harness;
using atlas::sim::testing::increment_values;
using atlas::sim::testing::random_into_counter;
using atlas::sim::testing::sum_into_counter;

namespace {

/// A finalised schedule with three named systems, so a `SystemId` can be turned into a name.
struct Fixture {
    Harness h;

    Fixture() {
        REQUIRE(h.schedule.add(increment_values(h.values)).has_value());
        REQUIRE(h.schedule.add(sum_into_counter(h.values, h.counter)).has_value());
        REQUIRE(h.schedule.add(random_into_counter(h.counter)).has_value());
        REQUIRE(h.schedule.finalise(h.world).has_value());
    }
};

const SystemId kIncrement = system_id("increment values");
const SystemId kSum = system_id("sum into counter");
const SystemId kRandom = system_id("random into counter");

}  // namespace

TEST_CASE("the first differing system is named", "[sim][divergence]") {
    const Fixture fixture;
    const std::array<SystemHash, 3> expected{{{kIncrement, 1}, {kSum, 2}, {kRandom, 3}}};
    const std::array<SystemHash, 3> actual{{{kIncrement, 1}, {kSum, 99}, {kRandom, 3}}};

    const auto divergence =
        attribute_divergence(42, 0xAAAA, 0xBBBB, expected, actual, fixture.h.schedule);

    CHECK(divergence.tick == 42);
    CHECK(divergence.expected_hash == 0xAAAA);
    CHECK(divergence.actual_hash == 0xBBBB);
    REQUIRE(divergence.first_system.has_value());
    CHECK(*divergence.first_system == kSum);
    CHECK(divergence.description.contains("sum into counter"));
    CHECK(divergence.description.contains("tick 42"));
}

TEST_CASE("systems are matched by identifier, not by position", "[sim][divergence]") {
    // Two runs of one schedule should list systems in the same order. Nothing guarantees it,
    // and matching by index would blame whichever system happened to sit at the same offset as
    // the one that actually differs.
    const Fixture fixture;
    const std::array<SystemHash, 3> expected{{{kIncrement, 1}, {kSum, 2}, {kRandom, 3}}};
    const std::array<SystemHash, 3> reversed{{{kRandom, 3}, {kSum, 99}, {kIncrement, 1}}};

    const auto divergence = attribute_divergence(7, 1, 2, expected, reversed, fixture.h.schedule);
    REQUIRE(divergence.first_system.has_value());
    CHECK(*divergence.first_system == kSum);
}

TEST_CASE("a system the other run never hashed is the first difference", "[sim][divergence]") {
    // A run that produced no hash for a system did not run the same schedule, which is worth
    // reporting rather than skipping past to find one that happens to be present and differ.
    const Fixture fixture;
    const std::array<SystemHash, 3> expected{{{kIncrement, 1}, {kSum, 2}, {kRandom, 3}}};
    const std::array<SystemHash, 2> missing{{{kIncrement, 1}, {kRandom, 3}}};

    const auto divergence = attribute_divergence(7, 1, 2, expected, missing, fixture.h.schedule);
    REQUIRE(divergence.first_system.has_value());
    CHECK(*divergence.first_system == kSum);
}

TEST_CASE("with no per-system hashes the divergence says it cannot be attributed",
          "[sim][divergence]") {
    const Fixture fixture;
    const auto divergence = attribute_divergence(7, 1, 2, {}, {}, fixture.h.schedule);
    CHECK_FALSE(divergence.first_system.has_value());
    CHECK(divergence.description.contains("no per-system hashes"));
}

TEST_CASE("a difference in a table no system writes says so", "[sim][divergence]") {
    // The case the code this replaced got wrong. Every system that recorded a hash agrees and
    // the state hashes still differ, and the old message said "no per-system hashes were
    // recorded" — which is a different situation with a different cause, and simply untrue
    // while somebody is looking at the hashes it claims are missing.
    const Fixture fixture;
    const std::array<SystemHash, 3> both{{{kIncrement, 1}, {kSum, 2}, {kRandom, 3}}};

    const auto divergence = attribute_divergence(7, 0xAAAA, 0xBBBB, both, both, fixture.h.schedule);
    CHECK_FALSE(divergence.first_system.has_value());
    CHECK_FALSE(divergence.description.contains("no per-system hashes"));
    CHECK(divergence.description.contains("no system declares that it writes"));
}

TEST_CASE("a differing system the schedule does not know is reported as such",
          "[sim][divergence]") {
    // Two runs that are not running the same systems at all, which is a bigger problem than a
    // divergence and should not be reported as an unattributable one.
    const Fixture fixture;
    const std::array<SystemHash, 1> expected{{{system_id("a system from another build"), 1}}};
    const std::array<SystemHash, 1> actual{{{kIncrement, 1}}};

    const auto divergence = attribute_divergence(7, 1, 2, expected, actual, fixture.h.schedule);
    REQUIRE(divergence.first_system.has_value());
    CHECK(divergence.description.contains("not in this schedule"));
}
