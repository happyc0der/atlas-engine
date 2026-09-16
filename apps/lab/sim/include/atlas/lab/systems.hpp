// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Four mock systems, one per access pattern a real simulation would have. None of them
/// means anything; each exists to exercise the kernel a particular way.
///
/// | System                | Pattern                                    | Reads                |
/// Writes     |
/// |-----------------------|--------------------------------------------|----------------------|------------|
/// | step region value     | linear iterate-and-mutate                  | -                    |
/// cells      | | drift owner index     | cross-table read plus a named random stream| population
/// | cells      | | accumulate population | indirection through adjacency, saturating  | cells,
/// adjacency     | population | | shift chunk owner     | a random stream and a visible change | -
/// | chunks     |
///
/// All integer arithmetic. There is no floating point in any authoritative path, so ADR-0008
/// needs no justification here; that is stated rather than left as an absence.
///
/// Each system's compute writes only its own scratch, captured by shared pointer as the
/// engine's own tests do, and its commit copies the scratch into the table it owns. Scratch
/// is reused across ticks, so a steady-state tick allocates nothing.
///
/// Thread affinity: whatever the kernel's is.

#include <atlas/core/result.hpp>
#include <atlas/lab/tables.hpp>
#include <atlas/simulation/schedule.hpp>

namespace atlas::lab {

/// How many cells drift per tick: one in this many.
inline constexpr std::uint32_t kDriftDivisor = 1024;

[[nodiscard]] sim::SystemDesc step_region_value(const TableIds& ids);
[[nodiscard]] sim::SystemDesc drift_owner_index(const TableIds& ids);
[[nodiscard]] sim::SystemDesc accumulate_population(const TableIds& ids);
[[nodiscard]] sim::SystemDesc shift_chunk_owner(const TableIds& ids);

/// All four, in the order above. Failure: whatever Schedule::add reports.
[[nodiscard]] Status add_lab_systems(sim::Schedule& schedule, const TableIds& ids);

}  // namespace atlas::lab
