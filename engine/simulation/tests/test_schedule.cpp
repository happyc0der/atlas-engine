// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/simulation/schedule.hpp>

#include "synthetic_tables.hpp"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <span>
#include <vector>

using atlas::ErrorCode;
using atlas::sim::Schedule;
using atlas::sim::system_id;
using atlas::sim::SystemDesc;
using atlas::sim::SystemId;
using atlas::sim::TableId;
using atlas::sim::World;
using atlas::sim::testing::CounterTable;

namespace {

/// A world with four interchangeable tables, so conflicts can be arranged freely.
struct Fixture {
    World world;
    TableId a;
    TableId b;
    TableId c;
    TableId d;

    Fixture() {
        a = world.add_table("a", std::make_unique<CounterTable>()).value();
        b = world.add_table("b", std::make_unique<CounterTable>()).value();
        c = world.add_table("c", std::make_unique<CounterTable>()).value();
        d = world.add_table("d", std::make_unique<CounterTable>()).value();
    }
};

/// A system that does nothing but declare what it touches.
[[nodiscard]] SystemDesc inert(std::string_view name, std::vector<TableId> reads,
                               std::vector<TableId> writes) {
    SystemDesc desc;
    desc.name = name;
    desc.reads = std::move(reads);
    desc.writes = std::move(writes);
    desc.compute = [](const atlas::sim::ComputeContext&) {};
    if (!desc.writes.empty()) {
        desc.commit = [](const atlas::sim::CommitContext&) {};
    }
    return desc;
}

}  // namespace

TEST_CASE("a system identifier comes from its name", "[sim][schedule]") {
    CHECK(system_id("movement") == system_id("movement"));
    CHECK(system_id("movement") != system_id("growth"));
    CHECK(atlas::sim::valid(system_id("movement")));
    CHECK_FALSE(atlas::sim::valid(SystemId::Invalid));
}

TEST_CASE("a system can be added and found", "[sim][schedule]") {
    const Fixture f;
    Schedule schedule;
    REQUIRE(schedule.add(inert("one", {f.a}, {f.b})).has_value());

    CHECK(schedule.size() == 1);
    CHECK(schedule.find(system_id("one")) != nullptr);
    CHECK(schedule.find(system_id("absent")) == nullptr);
}

TEST_CASE("a nameless or empty system is refused", "[sim][schedule]") {
    Schedule schedule;
    CHECK_FALSE(schedule.add(inert("", {}, {})).has_value());

    SystemDesc empty;
    empty.name = "does nothing";
    CHECK_FALSE(schedule.add(std::move(empty)).has_value());
}

TEST_CASE("a duplicate system name is refused", "[sim][schedule]") {
    const Fixture f;
    Schedule schedule;
    REQUIRE(schedule.add(inert("one", {}, {f.a})).has_value());

    const auto again = schedule.add(inert("one", {}, {f.b}));
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code() == ErrorCode::AlreadyExists);
}

TEST_CASE("a system that reads and writes the same table is refused", "[sim][schedule]") {
    // Its own ordering would be ambiguous: compute sees the value from before its own
    // commit, which is almost never what the author meant and cannot be seen from outside.
    const Fixture f;
    Schedule schedule;
    const auto added = schedule.add(inert("one", {f.a, f.b}, {f.a}));
    REQUIRE_FALSE(added.has_value());
    CHECK(added.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("a committing system must declare its writes", "[sim][schedule]") {
    // Otherwise the conflict analysis believes it touches nothing and schedules it beside
    // anything at all.
    const Fixture f;
    Schedule schedule;

    SystemDesc desc;
    desc.name = "sneaky";
    desc.reads = {f.a};
    desc.commit = [](const atlas::sim::CommitContext&) {};

    const auto added = schedule.add(std::move(desc));
    REQUIRE_FALSE(added.has_value());
    CHECK(added.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("declaring a table the world does not have is refused", "[sim][schedule]") {
    // A misspelled table name would otherwise conflict with nothing.
    const Fixture f;
    Schedule schedule;
    REQUIRE(schedule.add(inert("one", {}, {atlas::sim::table_id("typo")})).has_value());

    const auto status = schedule.finalise(f.world);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::NotFound);
    CHECK_FALSE(schedule.finalised());
}

TEST_CASE("a schedule must be finalised before its batches exist", "[sim][schedule]") {
    const Fixture f;
    Schedule schedule;
    REQUIRE(schedule.add(inert("one", {}, {f.a})).has_value());
    CHECK_FALSE(schedule.finalised());
    CHECK(schedule.batches().empty());

    REQUIRE(schedule.finalise(f.world).has_value());
    CHECK(schedule.finalised());
    CHECK(schedule.batches().size() == 1);
}

TEST_CASE("adding a system after finalising invalidates the batches", "[sim][schedule]") {
    const Fixture f;
    Schedule schedule;
    REQUIRE(schedule.add(inert("one", {}, {f.a})).has_value());
    REQUIRE(schedule.finalise(f.world).has_value());

    REQUIRE(schedule.add(inert("two", {}, {f.b})).has_value());
    CHECK_FALSE(schedule.finalised());
    CHECK(schedule.batches().empty());
}

TEST_CASE("systems touching nothing in common share a batch", "[sim][schedule]") {
    const Fixture f;
    Schedule schedule;
    REQUIRE(schedule.add(inert("one", {}, {f.a})).has_value());
    REQUIRE(schedule.add(inert("two", {}, {f.b})).has_value());
    REQUIRE(schedule.add(inert("three", {}, {f.c})).has_value());
    REQUIRE(schedule.finalise(f.world).has_value());

    REQUIRE(schedule.batches().size() == 1);
    CHECK(schedule.batches()[0].systems.size() == 3);
}

TEST_CASE("two readers of the same table share a batch", "[sim][schedule]") {
    // Nothing is being changed, so there is no conflict to separate them.
    const Fixture f;
    Schedule schedule;
    REQUIRE(schedule.add(inert("one", {f.a}, {f.b})).has_value());
    REQUIRE(schedule.add(inert("two", {f.a}, {f.c})).has_value());
    REQUIRE(schedule.finalise(f.world).has_value());

    CHECK(schedule.batches().size() == 1);
}

TEST_CASE("two writers of the same table are separated", "[sim][schedule]") {
    const Fixture f;
    Schedule schedule;
    REQUIRE(schedule.add(inert("one", {}, {f.a})).has_value());
    REQUIRE(schedule.add(inert("two", {}, {f.a})).has_value());
    REQUIRE(schedule.finalise(f.world).has_value());

    REQUIRE(schedule.batches().size() == 2);
    CHECK(schedule.batches()[0].systems == std::vector<std::size_t>{0});
    CHECK(schedule.batches()[1].systems == std::vector<std::size_t>{1});
}

TEST_CASE("a reader and a writer of the same table are separated", "[sim][schedule]") {
    const Fixture f;
    Schedule schedule;
    REQUIRE(schedule.add(inert("writer", {}, {f.a})).has_value());
    REQUIRE(schedule.add(inert("reader", {f.a}, {f.b})).has_value());
    REQUIRE(schedule.finalise(f.world).has_value());

    CHECK(schedule.batches().size() == 2);
}

TEST_CASE("a writer and a reader in the other order are also separated", "[sim][schedule]") {
    // The conflict is symmetric, and a one-sided check would miss this direction.
    const Fixture f;
    Schedule schedule;
    REQUIRE(schedule.add(inert("reader", {f.a}, {f.b})).has_value());
    REQUIRE(schedule.add(inert("writer", {}, {f.a})).has_value());
    REQUIRE(schedule.finalise(f.world).has_value());

    CHECK(schedule.batches().size() == 2);
}

TEST_CASE("batches follow the declared order", "[sim][schedule]") {
    // Reproducibility, not packing quality. A cleverer grouping could produce fewer batches
    // and would have to stay stable against unrelated edits to be worth having.
    const Fixture f;
    Schedule schedule;
    REQUIRE(schedule.add(inert("one", {}, {f.a})).has_value());
    REQUIRE(schedule.add(inert("two", {}, {f.a})).has_value());
    REQUIRE(schedule.add(inert("three", {}, {f.b})).has_value());
    REQUIRE(schedule.finalise(f.world).has_value());

    // "three" conflicts with neither, but it lands beside "two" because that is the batch
    // open when it is reached.
    REQUIRE(schedule.batches().size() == 2);
    CHECK(schedule.batches()[0].systems == std::vector<std::size_t>{0});
    CHECK(schedule.batches()[1].systems == std::vector<std::size_t>{1, 2});
}

TEST_CASE("every system appears in exactly one batch", "[sim][schedule]") {
    const Fixture f;
    Schedule schedule;
    REQUIRE(schedule.add(inert("one", {f.a}, {f.b})).has_value());
    REQUIRE(schedule.add(inert("two", {f.b}, {f.c})).has_value());
    REQUIRE(schedule.add(inert("three", {f.c}, {f.d})).has_value());
    REQUIRE(schedule.add(inert("four", {f.d}, {f.a})).has_value());
    REQUIRE(schedule.finalise(f.world).has_value());

    std::vector<std::size_t> seen;
    for (const auto& batch : schedule.batches()) {
        for (const std::size_t index : batch.systems) {
            seen.push_back(index);
        }
    }
    std::ranges::sort(seen);
    CHECK(seen == std::vector<std::size_t>{0, 1, 2, 3});
}

TEST_CASE("no batch contains a conflicting pair", "[sim][schedule]") {
    // The property that matters for M8: everything inside a batch must be safe to run at the
    // same time. Checked directly rather than inferred from the batch count.
    const Fixture f;
    Schedule schedule;
    REQUIRE(schedule.add(inert("one", {f.a}, {f.b})).has_value());
    REQUIRE(schedule.add(inert("two", {f.c}, {f.d})).has_value());
    REQUIRE(schedule.add(inert("three", {f.b}, {f.a})).has_value());
    REQUIRE(schedule.add(inert("four", {}, {f.c})).has_value());
    REQUIRE(schedule.finalise(f.world).has_value());

    const auto systems = schedule.systems();
    const auto overlaps = [](std::span<const TableId> x, std::span<const TableId> y) {
        return std::ranges::any_of(
            x, [y](const TableId id) { return std::ranges::find(y, id) != y.end(); });
    };

    for (const auto& batch : schedule.batches()) {
        for (std::size_t i = 0; i < batch.systems.size(); ++i) {
            for (std::size_t j = i + 1; j < batch.systems.size(); ++j) {
                const auto& x = systems[batch.systems[i]];
                const auto& y = systems[batch.systems[j]];
                INFO("'" << x.name << "' and '" << y.name << "' share a batch");
                CHECK_FALSE(overlaps(x.writes, y.writes));
                CHECK_FALSE(overlaps(x.writes, y.reads));
                CHECK_FALSE(overlaps(x.reads, y.writes));
            }
        }
    }
}

TEST_CASE("declarations are deduplicated", "[sim][schedule]") {
    const Fixture f;
    Schedule schedule;
    REQUIRE(schedule.add(inert("one", {f.a, f.a, f.a}, {f.b, f.b})).has_value());
    REQUIRE(schedule.finalise(f.world).has_value());

    const auto& system = schedule.systems()[0];
    CHECK(system.reads.size() == 1);
    CHECK(system.writes.size() == 1);
}

TEST_CASE("an empty schedule finalises to no batches", "[sim][schedule]") {
    const Fixture f;
    Schedule schedule;
    REQUIRE(schedule.finalise(f.world).has_value());
    CHECK(schedule.finalised());
    CHECK(schedule.batches().empty());
}
