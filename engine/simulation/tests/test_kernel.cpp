// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/simulation/kernel.hpp>

#include "synthetic_systems.hpp"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <vector>

using atlas::ErrorCode;
using atlas::sim::command_type;
using atlas::sim::CommandHandler;
using atlas::sim::CommandType;
using atlas::sim::Kernel;
using atlas::sim::KernelConfig;
using atlas::sim::SourceId;
using atlas::sim::TurnGate;
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
    CHECK(report->commands_rejected() == 1);
    // Late, not invalid. The payload was perfectly good; it named a tick that had gone. Under
    // lockstep that distinction ends a session, so it is counted apart rather than summed.
    CHECK(report->commands_late == 1);
    CHECK(report->commands_invalid == 0);
    CHECK(kernel.late_commands() == 1);
    CHECK(kernel.invalid_commands() == 0);
    CHECK(h.value_table().value[0] == 0);
}

TEST_CASE("a late command and an invalid one are counted apart", "[sim][kernel]") {
    // The header already says a command is validated again at apply time rather than trusted,
    // because it may have been submitted before a load replaced the state it referred to. This
    // uses that: the handler's answer changes between the submit and the tick, which is the
    // honest shape of a command that was good when it was sent and is not when it arrives.
    //
    // Under lockstep the two rejections mean opposite things. A late command is a protocol
    // violation and ends a session; an invalid one is ordinary and every peer refuses it
    // identically. Summing them, as the report did before M14, loses exactly that.
    Harness h;
    const auto accept = std::make_shared<bool>(true);
    CommandHandler handler;
    handler.validate = [accept](std::span<const std::byte> payload) -> atlas::Status {
        if (payload.size() != 4) {
            return std::unexpected(
                atlas::Error(ErrorCode::MalformedData, "expected a four-byte value"));
        }
        if (!*accept) {
            return std::unexpected(
                atlas::Error(ErrorCode::OutOfRange, "the state this referred to is gone"));
        }
        return atlas::ok();
    };
    handler.apply = [](World&, std::span<const std::byte>) {};
    REQUIRE(h.commands.register_handler(kSetFirst, std::move(handler)).has_value());
    REQUIRE(h.schedule.finalise(h.world).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{});
    REQUIRE(kernel.run(5).has_value());

    // One stamped for a tick that has gone, and one that will refuse itself on the way in.
    REQUIRE(h.commands.submit(2, SourceId{1}, kSetFirst, int_payload(9)).has_value());
    REQUIRE(h.commands.submit(5, SourceId{2}, kSetFirst, int_payload(9)).has_value());
    *accept = false;

    const auto report = kernel.step();
    REQUIRE(report.has_value());

    CHECK(report->commands_applied == 0);
    CHECK(report->commands_late == 1);
    CHECK(report->commands_invalid == 1);
    // And the total is still the total, so nothing that read the old field reads it wrongly.
    CHECK(report->commands_rejected() == 2);
    CHECK(kernel.late_commands() == 1);
    CHECK(kernel.invalid_commands() == 1);
}

TEST_CASE("a tick the gate has not cleared takes nothing out of the queue", "[sim][kernel]") {
    // The case that matters most in the whole slice. `drain` *removes* what it returns, so a
    // refusal placed after it would take this command out of the queue and throw it away with
    // the discarded report — and the retry would run the same tick with fewer commands and
    // reach a different state from every peer. Silent, and exactly the property lockstep exists
    // to provide.
    Harness h;
    REQUIRE(h.commands.register_handler(kSetFirst, set_first_handler(h.values)).has_value());
    REQUIRE(h.schedule.finalise(h.world).has_value());

    TurnGate gate;
    const std::array<SourceId, 1> peers{SourceId{1}};
    REQUIRE(gate.expect_sources(peers).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{.gate = &gate});
    REQUIRE(h.commands.submit(0, SourceId{1}, kSetFirst, int_payload(42)).has_value());
    const std::uint64_t hash_before = h.world.hash();

    CHECK_FALSE(kernel.ready());
    const auto refused = kernel.step();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == ErrorCode::Unavailable);
    // The message names who, because a stall that does not say what it is waiting for is a
    // stall nobody can act on.
    CHECK(refused.error().message().contains("source 1"));

    // Nothing moved: not the tick, not the world, not the queue.
    CHECK(kernel.current_tick() == 0);
    CHECK(h.world.hash() == hash_before);
    CHECK(kernel.stalled_steps() == 1);

    // And the command is still there to be applied once the turn arrives, which is what proves
    // the queue was not drained.
    REQUIRE(gate.mark_complete(SourceId{1}, 0).has_value());
    CHECK(kernel.ready());
    const auto report = kernel.step();
    REQUIRE(report.has_value());
    CHECK(report->commands_applied == 1);
    CHECK(h.value_table().value[0] == 42);
}

TEST_CASE("a gate expecting nobody is the run there always was", "[sim][kernel]") {
    // The solo guarantee at the level of the kernel rather than the gate: an empty expectation
    // set must be indistinguishable from no gate at all, or every golden hash moves.
    Harness h;
    REQUIRE(h.schedule.add(increment_values(h.values)).has_value());
    REQUIRE(h.schedule.finalise(h.world).has_value());

    // No gate at all, which is what every application that knows nothing about peers passes.
    // `ready()` is the function a driver is told to call before stepping, so this returning
    // false would freeze every solo run — and nothing else here would notice, because every
    // other case in this file constructs a gate.
    Kernel solo(h.world, h.schedule, h.commands, KernelConfig{.seed = 5});
    CHECK(solo.ready());
    CHECK(solo.ready_horizon() == TurnGate::kUnboundedHorizon);
    CHECK(solo.stalled_steps() == 0);

    const TurnGate empty;
    Kernel gated(h.world, h.schedule, h.commands, KernelConfig{.seed = 5, .gate = &empty});
    CHECK(gated.ready());
    CHECK(gated.ready_horizon() == TurnGate::kUnboundedHorizon);
    const auto gated_reports = gated.run(10);
    REQUIRE(gated_reports.has_value());
    CHECK(gated.stalled_steps() == 0);

    Harness other;
    REQUIRE(other.schedule.add(increment_values(other.values)).has_value());
    REQUIRE(other.schedule.finalise(other.world).has_value());
    Kernel ungated(other.world, other.schedule, other.commands, KernelConfig{.seed = 5});
    const auto ungated_reports = ungated.run(10);
    REQUIRE(ungated_reports.has_value());

    // Tick by tick rather than only at the end: a run that diverged and converged again
    // diverged.
    REQUIRE(gated_reports->size() == ungated_reports->size());
    for (std::size_t i = 0; i < gated_reports->size(); ++i) {
        INFO("tick " << i);
        CHECK((*gated_reports)[i].state_hash == (*ungated_reports)[i].state_hash);
    }
}

TEST_CASE("an unfinalised schedule is reported even while a peer is missing", "[sim][kernel]") {
    // Order matters. If the gate were consulted first, a real misconfiguration would be masked
    // for as long as a peer stayed missing, and whoever was debugging would go and look at the
    // network instead of at the schedule.
    Harness h;
    TurnGate gate;
    const std::array<SourceId, 1> peers{SourceId{1}};
    REQUIRE(gate.expect_sources(peers).has_value());

    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{.gate = &gate});
    const auto refused = kernel.step();
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == ErrorCode::InvalidArgument);
    CHECK(kernel.stalled_steps() == 0);
}

TEST_CASE("the ready horizon says how many ticks may run from here", "[sim][kernel]") {
    Harness h;
    REQUIRE(h.schedule.finalise(h.world).has_value());

    TurnGate gate;
    const std::array<SourceId, 2> peers{SourceId{1}, SourceId{2}};
    REQUIRE(gate.expect_sources(peers).has_value());
    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{.gate = &gate});

    CHECK(kernel.ready_horizon() == 0);
    for (atlas::Tick tick = 0; tick < 3; ++tick) {
        REQUIRE(gate.mark_complete(SourceId{1}, tick).has_value());
        REQUIRE(gate.mark_complete(SourceId{2}, tick).has_value());
    }
    // Half-open: three ticks are ready, so the horizon is 3 and the subtraction from the
    // current tick needs no adjustment.
    CHECK(kernel.ready_horizon() == 3);
    CHECK(kernel.ready_horizon() - kernel.current_tick() == 3);
    REQUIRE(kernel.run(3).has_value());
    CHECK(kernel.current_tick() == 3);
    CHECK_FALSE(kernel.ready());
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
