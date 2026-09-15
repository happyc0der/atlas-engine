// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/simulation/kernel.hpp>

#include "synthetic_systems.hpp"
#include <catch2/catch_test_macros.hpp>

#include <vector>

using atlas::ErrorCode;
using atlas::sim::command_type;
using atlas::sim::CommandHandler;
using atlas::sim::CommandType;
using atlas::sim::Kernel;
using atlas::sim::KernelConfig;
using atlas::sim::SourceId;
using atlas::sim::World;
using atlas::sim::testing::Harness;
using atlas::sim::testing::increment_values;
using atlas::sim::testing::random_into_counter;
using atlas::sim::testing::sum_into_counter;
using atlas::sim::testing::ValueTable;

namespace {

const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

const CommandType kSetFirst = command_type("set first value");

/// Sets the first row's value, so a test can see a command reach the state.
[[nodiscard]] CommandHandler set_first_handler(atlas::sim::TableId values) {
    CommandHandler handler;
    handler.validate = [](std::span<const std::byte> payload) -> atlas::Status {
        if (payload.size() != 4) {
            return std::unexpected(
                atlas::Error(ErrorCode::MalformedData, "expected a four-byte value"));
        }
        return atlas::ok();
    };
    handler.apply = [values](World& world, std::span<const std::byte> payload) {
        auto* table = dynamic_cast<ValueTable*>(world.table(values));
        if (table == nullptr || table->value.empty()) {
            return;
        }
        std::int32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            value |=
                static_cast<std::int32_t>(std::to_integer<std::uint32_t>(payload[i]) << (i * 8));
        }
        table->value[0] = value;
    };
    return handler;
}

[[nodiscard]] std::vector<std::byte> int_payload(std::int32_t value) {
    const auto raw = static_cast<std::uint32_t>(value);
    return {
        static_cast<std::byte>(raw & 0xFFU),
        static_cast<std::byte>((raw >> 8) & 0xFFU),
        static_cast<std::byte>((raw >> 16) & 0xFFU),
        static_cast<std::byte>((raw >> 24) & 0xFFU),
    };
}

}  // namespace

TEST_CASE("a kernel refuses to run an unfinalised schedule", "[sim][kernel]") {
    // Running it would mean running with declarations nobody checked.
    Harness h;
    REQUIRE(h.schedule.add(increment_values(h.values)).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{});
    const auto report = kernel.step();
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("a tick advances the counter and the state", "[sim][kernel]") {
    Harness h;
    REQUIRE(h.schedule.add(increment_values(h.values)).has_value());
    REQUIRE(h.schedule.finalise(h.world).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{});
    CHECK(kernel.current_tick() == 0);

    const auto report = kernel.step();
    REQUIRE(report.has_value());
    CHECK(report->tick == 0);
    CHECK(kernel.current_tick() == 1);
    CHECK(h.value_table().value[0] == 1);
}

TEST_CASE("an empty schedule still ticks and hashes", "[sim][kernel]") {
    Harness h;
    REQUIRE(h.schedule.finalise(h.world).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{});
    const auto report = kernel.step();
    REQUIRE(report.has_value());
    CHECK(report->state_hash == h.world.hash());
    CHECK(report->system_hashes.empty());
}

TEST_CASE("compute reads the state from before any commit", "[sim][kernel]") {
    // The two-phase contract. The summing system reads the values table while the
    // incrementing system has only written its own scratch, so it sees the tick's starting
    // values and not the ones about to be committed.
    Harness h(4);  // values 0, 1, 2, 3 summing to 6
    REQUIRE(h.schedule.add(increment_values(h.values)).has_value());
    REQUIRE(h.schedule.add(sum_into_counter(h.values, h.counter)).has_value());
    REQUIRE(h.schedule.finalise(h.world).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{});
    REQUIRE(kernel.step().has_value());

    CHECK(h.counter_table().count == 6);
    CHECK(h.value_table().value[0] == 1);
}

TEST_CASE("the same seed and schedule produce the same hashes", "[sim][kernel]") {
    // The claim M6 exists to make. Two runs of the same thing agree tick by tick.
    const auto run = [] {
        Harness h;
        REQUIRE(h.schedule.add(increment_values(h.values)).has_value());
        REQUIRE(h.schedule.add(sum_into_counter(h.values, h.counter)).has_value());
        REQUIRE(h.schedule.finalise(h.world).has_value());

        Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{.seed = 4242});
        auto reports = kernel.run(50);
        REQUIRE(reports.has_value());

        std::vector<std::uint64_t> hashes;
        for (const auto& report : *reports) {
            hashes.push_back(report.state_hash);
        }
        return hashes;
    };

    const auto first = run();
    const auto second = run();
    REQUIRE(first.size() == 50);
    CHECK(first == second);
}

TEST_CASE("the hashes actually change from tick to tick", "[sim][kernel]") {
    // Without this the test above would pass just as well on a kernel that never hashed
    // anything and returned zero every time.
    Harness h;
    REQUIRE(h.schedule.add(increment_values(h.values)).has_value());
    REQUIRE(h.schedule.finalise(h.world).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{});
    const auto reports = kernel.run(10);
    REQUIRE(reports.has_value());

    for (std::size_t i = 1; i < reports->size(); ++i) {
        CHECK((*reports)[i].state_hash != (*reports)[i - 1].state_hash);
    }
}

TEST_CASE("a different seed changes a run that uses randomness", "[sim][kernel]") {
    const auto run = [](std::uint64_t seed) {
        Harness h;
        REQUIRE(h.schedule.add(random_into_counter(h.counter)).has_value());
        REQUIRE(h.schedule.finalise(h.world).has_value());

        Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{.seed = seed});
        REQUIRE(kernel.run(20).has_value());
        return h.counter_table().count;
    };

    CHECK(run(1) != run(2));
    CHECK(run(1) == run(1));
}

TEST_CASE("per-system hashes attribute a change to the system that made it", "[sim][kernel]") {
    // A bare state hash says only that something differs. This says which system.
    Harness h;
    REQUIRE(h.schedule.add(increment_values(h.values)).has_value());
    REQUIRE(h.schedule.add(sum_into_counter(h.values, h.counter)).has_value());
    REQUIRE(h.schedule.finalise(h.world).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{});
    const auto reports = kernel.run(3);
    REQUIRE(reports.has_value());

    for (const auto& report : *reports) {
        REQUIRE(report.system_hashes.size() == 2);
        CHECK(report.system_hashes[0].system == atlas::sim::system_id("increment values"));
        CHECK(report.system_hashes[1].system == atlas::sim::system_id("sum into counter"));
    }

    // Both systems keep changing their own tables, so both sub-hashes move.
    CHECK((*reports)[0].system_hashes[0].hash != (*reports)[1].system_hashes[0].hash);
    CHECK((*reports)[0].system_hashes[1].hash != (*reports)[1].system_hashes[1].hash);
}

TEST_CASE("per-system hashes can be turned off", "[sim][kernel]") {
    Harness h;
    REQUIRE(h.schedule.add(increment_values(h.values)).has_value());
    REQUIRE(h.schedule.finalise(h.world).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{.record_system_hashes = false});
    const auto report = kernel.step();
    REQUIRE(report.has_value());
    CHECK(report->system_hashes.empty());
    CHECK(report->state_hash != 0);
}

TEST_CASE("a command reaches the state at the tick it named", "[sim][kernel]") {
    Harness h;
    REQUIRE(h.commands.register_handler(kSetFirst, set_first_handler(h.values)).has_value());
    REQUIRE(h.schedule.finalise(h.world).has_value());

    REQUIRE(h.commands.submit(3, SourceId::Local, kSetFirst, int_payload(77)).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{});

    // Nothing before its tick.
    REQUIRE(kernel.run(3).has_value());
    CHECK(h.value_table().value[0] == 0);

    const auto report = kernel.step();
    REQUIRE(report.has_value());
    CHECK(report->commands_applied == 1);
    CHECK(h.value_table().value[0] == 77);
}

TEST_CASE("commands apply before the systems run", "[sim][kernel]") {
    // The order the tick promises. A command lands, then the systems see it.
    Harness h(4);
    REQUIRE(h.commands.register_handler(kSetFirst, set_first_handler(h.values)).has_value());
    REQUIRE(h.schedule.add(sum_into_counter(h.values, h.counter)).has_value());
    REQUIRE(h.schedule.finalise(h.world).has_value());

    // Values start 0, 1, 2, 3 and sum to 6. Setting the first to 100 makes it 106.
    REQUIRE(h.commands.submit(0, SourceId::Local, kSetFirst, int_payload(100)).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{});
    REQUIRE(kernel.step().has_value());
    CHECK(h.counter_table().count == 106);
}

TEST_CASE("a command stamped for a tick already past is dropped and counted", "[sim][kernel]") {
    // Applying it would make the result depend on when it arrived, which is the thing being
    // avoided. Counting it means a late producer is visible rather than silent.
    Harness h;
    REQUIRE(h.commands.register_handler(kSetFirst, set_first_handler(h.values)).has_value());
    REQUIRE(h.schedule.finalise(h.world).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{});
    REQUIRE(kernel.run(5).has_value());

    REQUIRE(h.commands.submit(2, SourceId::Local, kSetFirst, int_payload(9)).has_value());
    const auto report = kernel.step();
    REQUIRE(report.has_value());

    CHECK(report->commands_applied == 0);
    CHECK(report->commands_rejected == 1);
    CHECK(kernel.late_commands() == 1);
    CHECK(h.value_table().value[0] == 0);
}

TEST_CASE("the tick can be moved, as a load does", "[sim][kernel]") {
    Harness h;
    REQUIRE(h.schedule.finalise(h.world).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{});
    kernel.set_tick(1000);
    CHECK(kernel.current_tick() == 1000);

    const auto report = kernel.step();
    REQUIRE(report.has_value());
    CHECK(report->tick == 1000);
    CHECK(kernel.current_tick() == 1001);
}

TEST_CASE("the tick is part of the random key", "[sim][kernel]") {
    // Two ticks never draw the same sequence, so a system doing the same work every tick
    // still gets different values.
    Harness h;
    REQUIRE(h.schedule.add(random_into_counter(h.counter)).has_value());
    REQUIRE(h.schedule.finalise(h.world).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{.seed = 5});
    const auto reports = kernel.run(20);
    REQUIRE(reports.has_value());

    // The counter accumulates a fresh draw each tick, so consecutive states differ.
    for (std::size_t i = 1; i < reports->size(); ++i) {
        CHECK((*reports)[i].state_hash != (*reports)[i - 1].state_hash);
    }
}

TEST_CASE("running many ticks returns one report each", "[sim][kernel]") {
    Harness h;
    REQUIRE(h.schedule.add(increment_values(h.values)).has_value());
    REQUIRE(h.schedule.finalise(h.world).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{});
    const auto reports = kernel.run(100);
    REQUIRE(reports.has_value());
    CHECK(reports->size() == 100);

    for (std::size_t i = 0; i < reports->size(); ++i) {
        CHECK((*reports)[i].tick == i);
    }
    CHECK(kernel.current_tick() == 100);
}

TEST_CASE("running zero ticks does nothing", "[sim][kernel]") {
    Harness h;
    REQUIRE(h.schedule.finalise(h.world).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{});
    const auto before = h.world.hash();
    const auto reports = kernel.run(0);
    REQUIRE(reports.has_value());
    CHECK(reports->empty());
    CHECK(kernel.current_tick() == 0);
    CHECK(h.world.hash() == before);
}
