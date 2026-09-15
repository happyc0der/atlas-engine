// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Recording what happened, and proving it happens the same way again.
///
/// A replay is the only honest test of determinism. Everything else checks that a rule was
/// followed; this checks the thing the rules exist for.
///
/// A recording holds the seed, the tick it started at, every command with its stamp, and the
/// state hash at intervals. Playing it back starts from the same state, feeds the commands at
/// the ticks they name, and compares hashes at the recorded checkpoints. A mismatch reports
/// the first tick that differed and, when per-system hashes were recorded, the first system
/// whose writes differed, because "something diverged at tick 4000" is not a starting point
/// for anybody.
///
/// **Determinism is claimed only for the same build on the same platform.** Replaying across
/// compilers or architectures is a measurement, not a guarantee. See docs/DETERMINISM.md.
///
/// Thread affinity: main thread, with the kernel.

#include <atlas/core/result.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/kernel.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace atlas::sim {

inline constexpr std::uint64_t kReplayMagic = 0x59'41'4C'50'52'41'56'00ULL;  // "\0VARPLAY"
inline constexpr std::uint32_t kReplayFormatVersion = 1;

/// A state hash recorded at a particular tick.
struct HashCheckpoint {
    Tick tick = 0;
    std::uint64_t state_hash = 0;
    /// Per-system hashes, when the kernel recorded them. Empty otherwise.
    std::vector<SystemHash> system_hashes;
};

/// A recorded run.
struct Replay {
    std::uint32_t format_version = kReplayFormatVersion;
    std::uint32_t hash_algorithm_version = kHashAlgorithmVersion;

    std::uint64_t seed = 0;
    Tick first_tick = 0;
    std::uint64_t tick_count = 0;

    /// The state hash before the first tick ran, so a playback starting from the wrong state
    /// is caught immediately rather than at the first checkpoint.
    std::uint64_t initial_state_hash = 0;

    /// Every command, in the order they were applied.
    std::vector<Command> commands;

    /// Hashes at intervals, always including the last tick.
    std::vector<HashCheckpoint> checkpoints;

    [[nodiscard]] std::vector<std::byte> to_bytes() const;
    [[nodiscard]] static Result<Replay> from_bytes(std::span<const std::byte> bytes);
};

/// Records a run as it happens.
///
/// Wraps a kernel rather than replacing it, so recording is something a run has rather than a
/// different way of running. That matters: a recorded run and an unrecorded one must be the
/// same run, and they are if recording only observes.
class ReplayRecorder {
  public:
    /// `checkpoint_interval` of 1 records every tick, which is what a test wants and what a
    /// long run cannot afford.
    ReplayRecorder(std::uint64_t seed, Tick first_tick, std::uint64_t initial_state_hash,
                   std::uint64_t checkpoint_interval = 1);

    /// Record the commands drained for a tick, before they are applied.
    void record_commands(std::span<const Command> commands);

    /// Record what a tick produced.
    void record_tick(const TickReport& report);

    [[nodiscard]] const Replay& replay() const noexcept { return m_replay; }

    [[nodiscard]] Replay take() noexcept { return std::move(m_replay); }

  private:
    Replay m_replay;
    std::uint64_t m_interval;
    std::uint64_t m_ticks_recorded = 0;
};

/// Where and how two runs stopped agreeing.
struct Divergence {
    Tick tick = 0;
    std::uint64_t expected_hash = 0;
    std::uint64_t actual_hash = 0;

    /// The first system whose writes differed, when both runs recorded per-system hashes.
    std::optional<SystemId> first_system;
    std::string description;
};

/// The outcome of a playback.
struct ReplayResult {
    std::uint64_t ticks_run = 0;
    std::uint64_t checkpoints_checked = 0;
    /// Empty when the replay reproduced exactly.
    std::optional<Divergence> divergence;

    [[nodiscard]] bool matched() const noexcept { return !divergence.has_value(); }
};

/// Play a recording back against a world already in the right starting state.
///
/// Fails, rather than reporting a divergence, when the setup is wrong: a schedule that was
/// never finalised, a command type with no handler, or a starting state whose hash does not
/// match the recording. Those are not divergences; they mean the replay was never going to
/// be a fair comparison.
[[nodiscard]] Result<ReplayResult> play(const Replay& replay, World& world, Schedule& schedule,
                                        CommandQueue& commands);

}  // namespace atlas::sim
