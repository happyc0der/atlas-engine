// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/lab/generate.hpp>
#include <atlas/lab/snapshot.hpp>

#include <catch2/catch_test_macros.hpp>

using atlas::lab::band;
using atlas::lab::CellSnapshot;
using atlas::lab::MapMode;

TEST_CASE("a snapshot carries every band at the layout's size", "[lab][snapshot]") {
    auto lab = atlas::lab::generate({.width = 8, .height = 8, .chunk_size = 4, .seed = 9}).value();
    const auto snapshot =
        atlas::lab::build_snapshot(lab.world, lab.ids, lab.layout, {.tick = 12, .state_hash = 34});
    CHECK(snapshot->header.tick == 12);
    CHECK(snapshot->layout == lab.layout);
    for (const MapMode mode : {MapMode::RegionValue, MapMode::OwnerIndex, MapMode::PopulationValue,
                               MapMode::ColorIndex}) {
        CHECK(band(*snapshot, mode).size() == 64);
    }
    CHECK(snapshot->chunk_owner.size() == 4);
    CHECK(snapshot->chunk_population.size() == 4);
}

TEST_CASE("bands are the tables reduced to bytes", "[lab][snapshot]") {
    auto lab = atlas::lab::generate({.width = 8, .height = 4, .chunk_size = 4, .seed = 9}).value();
    auto& cells = atlas::lab::cell_table(lab.world, lab.ids);
    auto& population = atlas::lab::population_table(lab.world, lab.ids);
    cells.region_value[3] = 0xAB'00'00'00U;
    cells.owner_index[3] = 5;
    cells.color_index[3] = 6;
    population.population_value[3] = atlas::lab::kPopulationCap;
    population.population_value[4] = 0;

    const auto snapshot = atlas::lab::build_snapshot(lab.world, lab.ids, lab.layout, {});
    CHECK(band(*snapshot, MapMode::RegionValue)[3] == 0xAB);
    CHECK(band(*snapshot, MapMode::OwnerIndex)[3] == 5);
    CHECK(band(*snapshot, MapMode::ColorIndex)[3] == 6);
    CHECK(band(*snapshot, MapMode::PopulationValue)[3] == 255);
    CHECK(band(*snapshot, MapMode::PopulationValue)[4] == 0);
}

TEST_CASE("chunk population totals are sums over the chunk's contiguous range", "[lab][snapshot]") {
    auto lab = atlas::lab::generate({.width = 8, .height = 4, .chunk_size = 4, .seed = 9}).value();
    auto& population = atlas::lab::population_table(lab.world, lab.ids);
    for (std::size_t i = 0; i < population.row_count(); ++i) {
        population.population_value[i] = i < 16 ? 1 : 10;  // chunk 0 then chunk 1
    }
    const auto snapshot = atlas::lab::build_snapshot(lab.world, lab.ids, lab.layout, {});
    CHECK(snapshot->chunk_population[0] == 16);
    CHECK(snapshot->chunk_population[1] == 160);
}

TEST_CASE("fill_snapshot reuses storage", "[lab][snapshot]") {
    const auto lab =
        atlas::lab::generate({.width = 8, .height = 4, .chunk_size = 4, .seed = 9}).value();
    CellSnapshot snapshot;
    atlas::lab::fill_snapshot(snapshot, lab.world, lab.ids, lab.layout, {});
    const auto* data = snapshot.color_band.data();
    atlas::lab::fill_snapshot(snapshot, lab.world, lab.ids, lab.layout, {.tick = 1});
    CHECK(snapshot.color_band.data() == data);
    CHECK(snapshot.header.tick == 1);
}

TEST_CASE("map modes cycle", "[lab][snapshot]") {
    STATIC_REQUIRE(atlas::lab::next(MapMode::RegionValue) == MapMode::OwnerIndex);
    STATIC_REQUIRE(atlas::lab::next(MapMode::ColorIndex) == MapMode::RegionValue);
}
