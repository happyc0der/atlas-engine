// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Where and how two runs of the same schedule stopped agreeing, and which system to blame.
///
/// "Something diverged at tick 4000" is not a starting point for anybody. Per-system hashes
/// exist so that a mismatch can be attributed rather than merely detected, and this is the
/// arithmetic that does the attributing.
///
/// **Extracted from inside `play()` in M14**, where it could only compare a recording against a
/// live run. Lockstep has two live runs and no recording at all: a peer sends the hashes it
/// computed, and the receiver compares them with its own. The shape of the comparison is
/// identical and the inputs are not, so the logic moved out rather than being copied — a second
/// attribution would agree today and disagree the first time either is fixed.
///
/// Thread affinity: none. These are pure functions over two lists.

#include <atlas/core/time.hpp>
#include <atlas/simulation/kernel.hpp>
#include <atlas/simulation/schedule.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace atlas::sim {

/// Where and how two runs stopped agreeing.
struct Divergence {
    Tick tick = 0;
    std::uint64_t expected_hash = 0;
    std::uint64_t actual_hash = 0;

    /// The first system whose writes differed, when both runs recorded per-system hashes.
    std::optional<SystemId> first_system;
    std::string description;
};

/// Work out which system first disagreed, and say so in a sentence.
///
/// `expected` is the reference side and `actual` the side under test. That is a choice rather
/// than a symmetry: in a replay the recording is the reference, and between two peers whichever
/// one is being checked against is. The description reads in those terms, so passing them the
/// wrong way round produces a sentence that blames the wrong machine.
///
/// **Matched by `SystemId`, never by position.** Two runs of the same schedule should list
/// systems in the same order, and if they do not, that is itself worth naming rather than a
/// reason to compare nothing: a system in `expected` with no entry in `actual` is reported as
/// the first difference, because a run that produced no hash for a system did not run the same
/// schedule.
///
/// Three outcomes, and the third is why this is not a straight move. When every system that
/// recorded a hash agrees and the state hashes still differ, the difference is in a table no
/// system declares it writes — and the description says exactly that, where the code this
/// replaced said "no per-system hashes were recorded", which in that case was simply untrue.
///
/// `schedule` is used only to turn a `SystemId` into a name for the description.
[[nodiscard]] Divergence attribute_divergence(Tick tick, std::uint64_t expected_state_hash,
                                              std::uint64_t actual_state_hash,
                                              std::span<const SystemHash> expected,
                                              std::span<const SystemHash> actual,
                                              const Schedule& schedule);

}  // namespace atlas::sim
