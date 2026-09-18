// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The one command type: the single path by which a click reaches state.
///
/// Five bytes: a little-endian cell index and a colour. The validator checks the length and
/// both ranges; apply decodes again and writes, so a command is never partly applied. The
/// cell bound is shared rather than captured by value because a load may change the grid, and
/// a validator bound to the old grid would accept indices the new one does not have.
///
/// Thread affinity: register on the main thread before the first tick; submit from the main
/// thread; apply happens inside the kernel.

#include <atlas/core/result.hpp>
#include <atlas/core/time.hpp>
#include <atlas/lab/tables.hpp>
#include <atlas/simulation/command.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace atlas::lab {

inline constexpr sim::CommandType kSetColorIndex = sim::command_type("set_color_index");
inline constexpr std::size_t kSetColorIndexBytes = 5;

struct SetColorIndex {
    std::uint32_t cell = 0;
    std::uint8_t color = 0;
};

using CellBound = std::atomic<std::uint32_t>;

[[nodiscard]] std::array<std::byte, kSetColorIndexBytes>
encode_set_color_index(std::uint32_t cell, std::uint8_t color) noexcept;

/// Failure: MalformedData for a wrong length, OutOfRange for a cell or colour past its bound.
[[nodiscard]] Result<SetColorIndex> decode_set_color_index(std::span<const std::byte> payload,
                                                           std::uint32_t cell_count);

/// Failure: whatever CommandQueue::register_handler reports.
[[nodiscard]] Status register_lab_commands(sim::CommandQueue& commands, const TableIds& ids,
                                           const std::shared_ptr<const CellBound>& cell_bound);

/// The payloads a deterministic run would submit for one tick.
///
/// Separated from the submit below in M14 because a lockstep peer needs the commands **in
/// hand**: it stamps them, submits them locally, and sends the same list to every other peer, so
/// it cannot hand them to a queue and then ask what it just handed over. One generator either
/// way, because two would drift and the whole point is that every peer produces the same bytes.
[[nodiscard]] std::vector<std::vector<std::byte>>
synthetic_payloads(Tick target, std::uint32_t count, std::uint64_t seed, std::uint32_t cell_count);

/// A deterministic command source for headless runs, without which a replay test proves far
/// less than it appears to: `count` set_color_index commands for `target`, drawn from a
/// stream keyed by seed and tick. Failure: whatever submit reports.
///
/// `source` is a parameter rather than `SourceId::Local`, which it was until M14. Under
/// lockstep `Local` is a *role* — it means peer zero, not "whoever is running this" — so a peer
/// that stamped `Local` would be signing another peer's name to its own commands, and the two
/// streams would collide in the total order that makes lockstep work. The stream the payloads
/// are drawn from is keyed by seed and tick only, so two peers asked for the same seed produce
/// the same commands under different names, which is exactly what a test wants.
[[nodiscard]] Status submit_synthetic_commands(sim::CommandQueue& commands, Tick target,
                                               std::uint32_t count, std::uint64_t seed,
                                               std::uint32_t cell_count, sim::SourceId source);

}  // namespace atlas::lab
