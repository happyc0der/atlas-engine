// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/lab/generate.hpp>
#include <atlas/simulation/rng.hpp>

#include <utility>

namespace atlas::lab {

void build_grid_adjacency(const GridLayout& layout, std::vector<std::uint32_t>& first,
                          std::vector<std::uint32_t>& neighbour) {
    const std::uint32_t cells = layout.cell_count();
    first.clear();
    neighbour.clear();
    first.reserve(std::size_t{cells} + 1);
    neighbour.reserve(std::size_t{cells} * 4);

    for (std::uint32_t index = 0; index < cells; ++index) {
        first.push_back(static_cast<std::uint32_t>(neighbour.size()));
        const CellCoords at = layout.cell_coords(index);
        if (at.x > 0) {
            neighbour.push_back(layout.cell_index(at.x - 1, at.y));
        }
        if (at.x + 1 < layout.width()) {
            neighbour.push_back(layout.cell_index(at.x + 1, at.y));
        }
        if (at.y > 0) {
            neighbour.push_back(layout.cell_index(at.x, at.y - 1));
        }
        if (at.y + 1 < layout.height()) {
            neighbour.push_back(layout.cell_index(at.x, at.y + 1));
        }
    }
    first.push_back(static_cast<std::uint32_t>(neighbour.size()));
}

void populate(sim::World& world, const TableIds& ids, const GridLayout& layout,
              std::uint64_t seed) {
    // Tick zero, stream per column: generation is a named draw like any other, so a change
    // to one column's distribution cannot shift another's.
    const sim::RngStreams streams(seed, 0);

    auto& grid = grid_table(world, ids);
    grid.width = layout.width();
    grid.height = layout.height();
    grid.chunk_size = layout.chunk_size();
    grid.seed = seed;

    const std::uint32_t cells = layout.cell_count();
    auto& cell = cell_table(world, ids);
    cell.resize(cells);
    {
        auto region = streams.stream("region_value");
        auto owner = streams.stream("owner_index");
        auto color = streams.stream("color_index");
        for (std::uint32_t i = 0; i < cells; ++i) {
            cell.region_value[i] = region.next_u32();
            cell.owner_index[i] = static_cast<std::uint16_t>(owner.next_below(kOwnerCount));
            cell.color_index[i] = static_cast<std::uint8_t>(color.next_below(kColorCount));
        }
    }

    auto& population = population_table(world, ids);
    population.resize(cells);
    {
        auto stream = streams.stream("population_value");
        for (std::uint32_t i = 0; i < cells; ++i) {
            population.population_value[i] =
                static_cast<std::uint32_t>(stream.next_below(kPopulationCap / 4));
        }
    }

    auto& chunks = chunk_table(world, ids);
    chunks.resize(layout.chunk_count());
    {
        auto stream = streams.stream("chunk_owner");
        for (std::uint32_t i = 0; i < layout.chunk_count(); ++i) {
            chunks.owner_index[i] = static_cast<std::uint16_t>(stream.next_below(kOwnerCount));
        }
    }

    std::vector<std::uint32_t> first;
    std::vector<std::uint32_t> neighbour;
    build_grid_adjacency(layout, first, neighbour);
    const auto set = adjacency_table(world, ids).set(std::move(first), std::move(neighbour));
    ATLAS_ASSERT_MSG(set.has_value(), "a generated grid adjacency failed its own invariants");
}

Result<LabWorld> generate(const LabConfig& config) {
    auto layout = GridLayout::create(config.width, config.height, config.chunk_size);
    if (!layout) {
        return std::unexpected(std::move(layout).error().context("the lab grid"));
    }
    LabWorld lab;
    auto ids = add_lab_tables(lab.world);
    if (!ids) {
        return std::unexpected(std::move(ids).error().context("adding the lab tables"));
    }
    lab.ids = *ids;
    lab.layout = *layout;
    populate(lab.world, lab.ids, lab.layout, config.seed);
    return lab;
}

}  // namespace atlas::lab
