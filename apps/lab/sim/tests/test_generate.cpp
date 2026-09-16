// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/lab/generate.hpp>

#include <catch2/catch_test_macros.hpp>

using atlas::lab::generate;
using atlas::lab::GridLayout;
using atlas::lab::LabConfig;

TEST_CASE("generation fills every table to the layout's size", "[lab][generate]") {
    const LabConfig config{.width = 8, .height = 8, .chunk_size = 4, .seed = 3};
    auto lab = generate(config).value();
    CHECK(lab.layout.cell_count() == 64);
    CHECK(atlas::lab::cell_table(lab.world, lab.ids).row_count() == 64);
    CHECK(atlas::lab::population_table(lab.world, lab.ids).row_count() == 64);
    CHECK(atlas::lab::chunk_table(lab.world, lab.ids).row_count() == 4);
    CHECK(atlas::lab::adjacency_table(lab.world, lab.ids).row_count() == 64);
    CHECK(atlas::lab::grid_table(lab.world, lab.ids).seed == 3);
    CHECK(atlas::lab::validate_world(lab.world, lab.ids).has_value());
}

TEST_CASE("grid adjacency has corner, edge and interior degrees", "[lab][generate]") {
    const auto layout = GridLayout::create(4, 4, 2).value();
    std::vector<std::uint32_t> first;
    std::vector<std::uint32_t> neighbour;
    atlas::lab::build_grid_adjacency(layout, first, neighbour);
    REQUIRE(first.size() == 17);
    // 4 corners × 2 + 8 edges × 3 + 4 interior × 4 = 48 directed edges.
    CHECK(neighbour.size() == 48);
    const auto degree = [&](std::uint32_t x, std::uint32_t y) {
        const std::uint32_t i = layout.cell_index(x, y);
        return first[i + 1] - first[i];
    };
    CHECK(degree(0, 0) == 2);
    CHECK(degree(1, 0) == 3);
    CHECK(degree(1, 1) == 4);
    CHECK(degree(3, 3) == 2);
    // Neighbours cross chunk boundaries: (1,1) is in chunk 0, (2,1) in chunk 1.
    const std::uint32_t i = layout.cell_index(1, 1);
    bool crosses = false;
    for (std::uint32_t k = first[i]; k < first[i + 1]; ++k) {
        crosses = crosses || neighbour[k] == layout.cell_index(2, 1);
    }
    CHECK(crosses);
}

TEST_CASE("the same seed generates the same world and a different seed does not",
          "[lab][generate]") {
    const LabConfig config{.width = 16, .height = 8, .chunk_size = 4, .seed = 42};
    const auto a = generate(config).value().world.hash();
    const auto b = generate(config).value().world.hash();
    CHECK(a == b);
    LabConfig other = config;
    other.seed = 43;
    CHECK(generate(other).value().world.hash() != a);
}

TEST_CASE("generated values respect their ranges", "[lab][generate]") {
    auto lab = generate({.width = 32, .height = 32, .chunk_size = 8, .seed = 5}).value();
    const auto& cells = atlas::lab::cell_table(lab.world, lab.ids);
    const auto& population = atlas::lab::population_table(lab.world, lab.ids);
    for (std::size_t i = 0; i < cells.row_count(); ++i) {
        REQUIRE(cells.owner_index[i] < atlas::lab::kOwnerCount);
        REQUIRE(cells.color_index[i] < atlas::lab::kColorCount);
        REQUIRE(population.population_value[i] <= atlas::lab::kPopulationCap);
    }
}
