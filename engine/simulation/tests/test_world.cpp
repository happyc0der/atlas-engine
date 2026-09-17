// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/simulation/world.hpp>

#include "synthetic_tables.hpp"
#include <catch2/catch_test_macros.hpp>

#include <memory>

using atlas::ErrorCode;
using atlas::sim::table_id;
using atlas::sim::TableId;
using atlas::sim::World;
using atlas::sim::testing::CounterTable;
using atlas::sim::testing::ValueTable;

namespace {

/// A world with two tables and known contents.
struct Fixture {
    World world;
    TableId values = TableId::Invalid;
    TableId counter = TableId::Invalid;

    Fixture() {
        auto value_table = std::make_unique<ValueTable>();
        value_table->resize(4);
        value_table->value = {1, 2, 3, 4};
        value_table->owner_index = {10, 20, 30, 40};

        values = world.add_table("values", std::move(value_table)).value();
        counter = world.add_table("counter", std::make_unique<CounterTable>()).value();
    }
};

}  // namespace

TEST_CASE("a table identifier comes from its name", "[sim][world]") {
    // From the name and not from registration order, so a save written by one build reads in
    // another that registers its tables differently.
    CHECK(table_id("values") == table_id("values"));
    CHECK(table_id("values") != table_id("counter"));
    CHECK(atlas::sim::valid(table_id("values")));
    CHECK_FALSE(atlas::sim::valid(TableId::Invalid));
}

TEST_CASE("a table can be registered and found", "[sim][world]") {
    Fixture f;
    CHECK(f.world.table_count() == 2);
    CHECK(f.world.contains(f.values));
    CHECK(f.world.table(f.values) != nullptr);
    CHECK(f.world.table(table_id("absent")) == nullptr);
}

TEST_CASE("registering the same name twice is refused", "[sim][world]") {
    Fixture f;
    const auto again = f.world.add_table("values", std::make_unique<ValueTable>());
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code() == ErrorCode::AlreadyExists);
}

TEST_CASE("a nameless or null table is refused", "[sim][world]") {
    World world;
    CHECK_FALSE(world.add_table("", std::make_unique<ValueTable>()).has_value());
    CHECK_FALSE(world.add_table("something", nullptr).has_value());
}

TEST_CASE("tables are ordered by identifier, not by registration", "[sim][world]") {
    // The property the canonical hash rests on. Two worlds holding the same tables must
    // present them in the same order however they were assembled.
    World forwards;
    REQUIRE(forwards.add_table("alpha", std::make_unique<CounterTable>()).has_value());
    REQUIRE(forwards.add_table("beta", std::make_unique<CounterTable>()).has_value());
    REQUIRE(forwards.add_table("gamma", std::make_unique<CounterTable>()).has_value());

    World backwards;
    REQUIRE(backwards.add_table("gamma", std::make_unique<CounterTable>()).has_value());
    REQUIRE(backwards.add_table("beta", std::make_unique<CounterTable>()).has_value());
    REQUIRE(backwards.add_table("alpha", std::make_unique<CounterTable>()).has_value());

    const auto forward_ids = forwards.ids();
    const auto backward_ids = backwards.ids();
    REQUIRE(forward_ids.size() == backward_ids.size());
    for (std::size_t i = 0; i < forward_ids.size(); ++i) {
        CHECK(forward_ids[i] == backward_ids[i]);
    }

    // And ascending, so the order is a property of the identifiers rather than an accident.
    for (std::size_t i = 1; i < forward_ids.size(); ++i) {
        CHECK(forward_ids[i - 1] < forward_ids[i]);
    }
}

TEST_CASE("the hash does not depend on registration order", "[sim][world]") {
    const auto build = [](bool forwards) {
        World world;
        const auto add_values = [&world] {
            auto table = std::make_unique<ValueTable>();
            table->resize(3);
            table->value = {7, 8, 9};
            table->owner_index = {1, 2, 3};
            REQUIRE(world.add_table("values", std::move(table)).has_value());
        };
        const auto add_counter = [&world] {
            auto table = std::make_unique<CounterTable>();
            table->count = 42;
            REQUIRE(world.add_table("counter", std::move(table)).has_value());
        };

        if (forwards) {
            add_values();
            add_counter();
        } else {
            add_counter();
            add_values();
        }
        return world.hash();
    };

    CHECK(build(true) == build(false));
}

TEST_CASE("the hash changes when any value changes", "[sim][world]") {
    Fixture f;
    const std::uint64_t before = f.world.hash();

    auto* table = dynamic_cast<ValueTable*>(f.world.table(f.values));
    REQUIRE(table != nullptr);
    table->value[2] += 1;

    CHECK(f.world.hash() != before);
}

TEST_CASE("the hash distinguishes tables with the same contents", "[sim][world]") {
    // The identifier goes into the hash as well as the contents, so moving a row from one
    // table to another changes the state even when the bytes do not.
    World left;
    auto a = std::make_unique<CounterTable>();
    a->count = 5;
    REQUIRE(left.add_table("alpha", std::move(a)).has_value());
    REQUIRE(left.add_table("beta", std::make_unique<CounterTable>()).has_value());

    World right;
    REQUIRE(right.add_table("alpha", std::make_unique<CounterTable>()).has_value());
    auto b = std::make_unique<CounterTable>();
    b->count = 5;
    REQUIRE(right.add_table("beta", std::move(b)).has_value());

    CHECK(left.hash() != right.hash());
}

TEST_CASE("an added empty table changes the hash", "[sim][world]") {
    // The table count is hashed first, so a state with an extra empty table is a different
    // state and not an identical one.
    World one;
    REQUIRE(one.add_table("alpha", std::make_unique<CounterTable>()).has_value());

    World two;
    REQUIRE(two.add_table("alpha", std::make_unique<CounterTable>()).has_value());
    REQUIRE(two.add_table("beta", std::make_unique<CounterTable>()).has_value());

    CHECK(one.hash() != two.hash());
}

TEST_CASE("a table hashes on its own for attribution", "[sim][world]") {
    Fixture f;
    const auto values_before = f.world.table_hash(f.values).value();
    const auto counter_before = f.world.table_hash(f.counter).value();

    auto* table = dynamic_cast<CounterTable*>(f.world.table(f.counter));
    REQUIRE(table != nullptr);
    table->count = 99;

    // Only the table that changed changes. Without this a divergence could not be attributed
    // to the system that wrote it.
    CHECK(f.world.table_hash(f.values).value() == values_before);
    CHECK(f.world.table_hash(f.counter).value() != counter_before);
}

TEST_CASE("hashing an unknown table is an error", "[sim][world]") {
    const Fixture f;
    const auto hashed = f.world.table_hash(table_id("absent"));
    REQUIRE_FALSE(hashed.has_value());
    CHECK(hashed.error().code() == ErrorCode::NotFound);
}

TEST_CASE("clearing empties the tables but keeps them registered", "[sim][world]") {
    // The schedule refers to tables by identifier, so a load must change what is in them and
    // never what exists.
    Fixture f;
    f.world.clear();

    CHECK(f.world.table_count() == 2);
    CHECK(f.world.contains(f.values));
    CHECK(f.world.table(f.values)->row_count() == 0);
}

TEST_CASE("table information reports names and row counts", "[sim][world]") {
    const Fixture f;
    const auto tables = f.world.tables();
    REQUIRE(tables.size() == 2);

    // Ordered by identifier, like everything else observable.
    for (std::size_t i = 1; i < tables.size(); ++i) {
        CHECK(tables[i - 1].id < tables[i].id);
    }

    std::size_t total = 0;
    for (const auto& info : tables) {
        CHECK_FALSE(info.name.empty());
        total += info.row_count;
    }
    CHECK(total == 5);  // four value rows and the single counter row
}
