// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// What the renderer reads: an immutable copy of the world reduced to bytes.
///
/// Four palette bands, one byte per cell each, plus per-chunk derived totals. A map mode is
/// a (band, palette) pair, so switching mode changes which span the draw loop reads and
/// nothing is rebuilt. That is why all four bands are carried rather than the current one:
/// the requirement is that a mode switch costs nothing, and the cheapest switch is the one
/// that was already computed.
///
/// Published at most once per frame, after the last tick, never headless. Four bands over a
/// million cells is four megabytes; per tick at sixty a second would be a quarter of a
/// gigabyte per second of copying that no consumer reads, because the channel is latest-wins.
///
/// Thread affinity: built on the simulation side; read by anyone holding the shared pointer.

#include <atlas/core/time.hpp>
#include <atlas/lab/grid_layout.hpp>
#include <atlas/lab/tables.hpp>
#include <atlas/simulation/snapshot.hpp>
#include <atlas/simulation/world.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace atlas::lab {

enum class MapMode : std::uint8_t {
    RegionValue,
    OwnerIndex,
    PopulationValue,
    ColorIndex,
    Count,
};

[[nodiscard]] std::string_view to_string(MapMode mode) noexcept;

[[nodiscard]] constexpr MapMode next(MapMode mode) noexcept {
    const auto raw = static_cast<std::uint8_t>(mode) + 1U;
    return raw >= static_cast<std::uint8_t>(MapMode::Count) ? MapMode::RegionValue
                                                            : static_cast<MapMode>(raw);
}

struct CellSnapshot {
    sim::SnapshotHeader header;
    GridLayout layout;
    std::vector<std::uint8_t> region_band;
    std::vector<std::uint8_t> owner_band;
    std::vector<std::uint8_t> population_band;
    std::vector<std::uint8_t> color_band;
    /// Derived per chunk, which is why they are here and not in a table.
    std::vector<std::uint16_t> chunk_owner;
    std::vector<std::uint64_t> chunk_population;
};

[[nodiscard]] std::span<const std::uint8_t> band(const CellSnapshot& snapshot,
                                                 MapMode mode) noexcept;

/// Fill `out` from the world, reusing its storage. Allocation-free once the vectors have
/// reached the layout's size.
void fill_snapshot(CellSnapshot& out, const sim::World& world, const TableIds& ids,
                   const GridLayout& layout, sim::SnapshotHeader header);

/// A fresh snapshot, for callers that do not pool.
[[nodiscard]] std::shared_ptr<const CellSnapshot> build_snapshot(const sim::World& world,
                                                                 const TableIds& ids,
                                                                 const GridLayout& layout,
                                                                 sim::SnapshotHeader header);

}  // namespace atlas::lab
