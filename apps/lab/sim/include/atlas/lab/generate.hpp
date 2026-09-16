// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// A world from a seed. The same seed and shape produce the same world, byte for byte; that
/// is a tested property, not an intention.
///
/// Thread affinity: main thread, at startup.

#include <atlas/core/result.hpp>
#include <atlas/lab/grid_layout.hpp>
#include <atlas/lab/tables.hpp>
#include <atlas/simulation/world.hpp>

#include <cstdint>

namespace atlas::lab {

struct LabConfig {
    std::uint32_t width = 1024;
    std::uint32_t height = 1024;
    std::uint32_t chunk_size = 32;
    std::uint64_t seed = 1;
};

/// A world with its five tables filled, plus the ids and layout everything else needs.
struct LabWorld {
    sim::World world;
    TableIds ids;
    GridLayout layout;
};

/// Failure: InvalidArgument from the layout, or whatever adding a table reports.
[[nodiscard]] Result<LabWorld> generate(const LabConfig& config);

/// Fill already-added tables for a layout and seed. Exposed for tests and for regeneration
/// into an existing world; generate() is this plus the tables.
void populate(sim::World& world, const TableIds& ids, const GridLayout& layout, std::uint64_t seed);

/// The four-neighbour adjacency of a layout, in chunk-major cell indices, neighbours in the
/// fixed order left, right, up, down. Deterministic in the layout alone.
void build_grid_adjacency(const GridLayout& layout, std::vector<std::uint32_t>& first,
                          std::vector<std::uint32_t>& neighbour);

}  // namespace atlas::lab
