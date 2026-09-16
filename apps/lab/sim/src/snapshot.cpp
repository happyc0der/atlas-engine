// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/lab/snapshot.hpp>

namespace atlas::lab {

std::string_view to_string(MapMode mode) noexcept {
    switch (mode) {
    case MapMode::RegionValue: return "region value";
    case MapMode::OwnerIndex: return "owner index";
    case MapMode::PopulationValue: return "population value";
    case MapMode::ColorIndex: return "colour index";
    case MapMode::Count: break;
    }
    return "invalid";
}

std::span<const std::uint8_t> band(const CellSnapshot& snapshot, MapMode mode) noexcept {
    switch (mode) {
    case MapMode::RegionValue: return snapshot.region_band;
    case MapMode::OwnerIndex: return snapshot.owner_band;
    case MapMode::PopulationValue: return snapshot.population_band;
    case MapMode::ColorIndex: return snapshot.color_band;
    case MapMode::Count: break;
    }
    return {};
}

void fill_snapshot(CellSnapshot& out, const sim::World& world, const TableIds& ids,
                   const GridLayout& layout, sim::SnapshotHeader header) {
    out.header = header;
    out.layout = layout;

    const auto& cells = cell_table(world, ids);
    const auto& population = population_table(world, ids);
    const auto& chunks = chunk_table(world, ids);
    const std::size_t count = layout.cell_count();

    out.region_band.resize(count);
    out.owner_band.resize(count);
    out.population_band.resize(count);
    out.color_band.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        // The top byte, so consecutive ticks move the colour visibly rather than flickering
        // the low bits.
        out.region_band[i] = static_cast<std::uint8_t>(cells.region_value[i] >> 24U);
        out.owner_band[i] = static_cast<std::uint8_t>(cells.owner_index[i]);
        // Scaled into a byte against the cap, in 64-bit so the multiply cannot overflow.
        out.population_band[i] = static_cast<std::uint8_t>(
            (static_cast<std::uint64_t>(population.population_value[i]) * 255U) / kPopulationCap);
        out.color_band[i] = cells.color_index[i];
    }

    const std::size_t chunk_count = layout.chunk_count();
    out.chunk_owner.assign(chunks.owner_index.begin(), chunks.owner_index.end());
    out.chunk_population.assign(chunk_count, 0);
    for (std::size_t chunk = 0; chunk < chunk_count; ++chunk) {
        const std::size_t first = layout.chunk_first_cell(static_cast<std::uint32_t>(chunk));
        std::uint64_t total = 0;
        for (std::size_t i = first; i < first + layout.cells_per_chunk(); ++i) {
            total += population.population_value[i];
        }
        out.chunk_population[chunk] = total;
    }
}

std::shared_ptr<const CellSnapshot> build_snapshot(const sim::World& world, const TableIds& ids,
                                                   const GridLayout& layout,
                                                   sim::SnapshotHeader header) {
    auto snapshot = std::make_shared<CellSnapshot>();
    fill_snapshot(*snapshot, world, ids, layout, header);
    return snapshot;
}

}  // namespace atlas::lab
