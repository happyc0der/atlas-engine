// SPDX-License-Identifier: GPL-3.0-or-later
#include "lab_harness.hpp"
#include <catch2/catch_test_macros.hpp>

#include <cstdio>

using atlas::lab::testing::kSmall;
using atlas::lab::testing::LabHarness;

TEST_CASE("the schedule accepts the four systems and finalises", "[lab][systems]") {
    LabHarness h(kSmall);
    REQUIRE(h.schedule.size() == 4);
    REQUIRE(h.schedule.finalised());

    // Observed, then asserted: the plan forbids predicting the batching. What follows is what
    // finalise produced on the first run, printed so the next reader can see it too.
    std::printf("lab schedule: %zu batches\n", h.schedule.batches().size());
    for (std::size_t b = 0; b < h.schedule.batches().size(); ++b) {
        std::printf("  batch %zu:", b);
        for (const std::size_t index : h.schedule.batches()[b].systems) {
            std::printf(" [%s]", h.schedule.systems()[index].name.c_str());
        }
        std::printf("\n");
    }

    // Three batches. The two writers of `cells` serialise because the schedule's granularity
    // is the table, not the column: step region value touches region_value and drift owner
    // index touches owner_index, and a column-level write set would let them share a batch.
    // Recorded in docs/DEFERRED.md as an M8 candidate rather than acted on from one
    // observation. accumulate population must follow both because it reads `cells`; shift
    // chunk owner is independent and lands wherever the scheduler puts it.
    const auto& batches = h.schedule.batches();
    REQUIRE(batches.size() == 3);
    const auto name_of = [&](std::size_t batch, std::size_t slot) {
        return h.schedule.systems()[batches[batch].systems[slot]].name;
    };
    REQUIRE(batches[0].systems.size() == 1);
    CHECK(name_of(0, 0) == "step region value");
    REQUIRE(batches[1].systems.size() == 1);
    CHECK(name_of(1, 0) == "drift owner index");
    REQUIRE(batches[2].systems.size() == 2);
    CHECK(name_of(2, 0) == "accumulate population");
    CHECK(name_of(2, 1) == "shift chunk owner");
}

TEST_CASE("each system changes only the table it owns", "[lab][systems]") {
    LabHarness h(kSmall);
    auto kernel = h.kernel(11);
    const auto before_adjacency = h.lab.world.table_hash(h.lab.ids.adjacency).value();
    const auto before_grid = h.lab.world.table_hash(h.lab.ids.grid).value();
    const auto before_cells = h.lab.world.table_hash(h.lab.ids.cells).value();
    const auto before_population = h.lab.world.table_hash(h.lab.ids.population).value();

    REQUIRE(kernel.step().has_value());

    CHECK(h.lab.world.table_hash(h.lab.ids.adjacency).value() == before_adjacency);
    CHECK(h.lab.world.table_hash(h.lab.ids.grid).value() == before_grid);
    CHECK(h.lab.world.table_hash(h.lab.ids.cells).value() != before_cells);
    CHECK(h.lab.world.table_hash(h.lab.ids.population).value() != before_population);
}

TEST_CASE("population saturates at the cap and never exceeds it", "[lab][systems]") {
    LabHarness h(kSmall);
    auto& population = atlas::lab::population_table(h.lab.world, h.lab.ids);
    for (auto& v : population.population_value) {
        v = atlas::lab::kPopulationCap - 1;
    }
    auto kernel = h.kernel(1);
    REQUIRE(kernel.run(3).has_value());
    for (const auto v : population.population_value) {
        REQUIRE(v <= atlas::lab::kPopulationCap);
    }
    CHECK(population.population_value[0] == atlas::lab::kPopulationCap);
}

TEST_CASE("a tick changes exactly one chunk owner", "[lab][systems]") {
    LabHarness h(kSmall);
    const auto before = atlas::lab::chunk_table(h.lab.world, h.lab.ids).owner_index;
    auto kernel = h.kernel(2);
    REQUIRE(kernel.step().has_value());
    const auto& after = atlas::lab::chunk_table(h.lab.world, h.lab.ids).owner_index;
    std::size_t changed = 0;
    for (std::size_t i = 0; i < before.size(); ++i) {
        changed += before[i] != after[i] ? 1 : 0;
    }
    // Zero when the stream happened to pick the owner it already had; never more than one.
    CHECK(changed <= 1);
}
