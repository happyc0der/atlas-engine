// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// What a mod is allowed to read of this application's world.
///
/// **The application's, not the engine's.** ADR-0010 D7 and ADR-0015 D5: the engine has no game
/// state to describe — `sim::Table` exposes a row count and how to hash itself, and no accessor
/// at all — so what the bytes in a view *mean* is decided here, in the application that defines
/// the tables. The engine provides the mechanism and the bounds.
///
/// **Taken at a tick boundary, never from the snapshot channel.** The snapshot is latest-wins,
/// published once a frame, and only when there is a graphics device; a mod reading it would
/// decide by frame rate and would read nothing at all headless. These come straight from the
/// world between ticks, so every peer sees the same bytes for the same tick.
///
/// Lifetime: the colour band spans the world's own storage and is valid until the next tick
/// changes it. Nothing here copies a megabyte per tick, which is the reason it is a span.

#include <atlas/lab/grid_layout.hpp>
#include <atlas/lab/tables.hpp>
#include <atlas/simulation/world.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace atlas::lab {

/// The grid's shape, as twelve little-endian bytes: cell count, width, height.
///
/// A mod reads the cell count before it picks a cell, so the same mod works on any grid the lab
/// is started with. Fixed width and little-endian because that is what every platform this runs
/// on reads natively and what a guest can decode with three instructions.
struct LayoutView {
    std::array<std::byte, 12> bytes{};
};

[[nodiscard]] LayoutView layout_view(const GridLayout& layout) noexcept;

/// The colour of every cell, one byte each, in cell order.
///
/// Spanned out of the table rather than copied: at a million cells a copy would cost more per
/// tick than every mod put together, and the bytes are already in exactly the shape a mod wants.
[[nodiscard]] std::span<const std::byte> color_view(const sim::World& world,
                                                    const TableIds& ids) noexcept;

}  // namespace atlas::lab
