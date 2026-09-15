// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The tick: what happens, and in what order.
///
/// One tick is four steps, always in this order:
///
/// 1. **Drain commands** stamped for this tick, in `(source, sequence)` order, and apply
///    them. Everything from outside enters here and nowhere else.
/// 2. **Compute**, batch by batch, each system reading a `const World` and writing only
///    storage it owns.
/// 3. **Commit**, system by system in declared order, each applying what it computed.
/// 4. **Hash** the resulting state.
///
/// Compute and commit are separate because that is what lets compute move onto worker
/// threads in M8 without changing a result. In M6 both run on the main thread, one system at
/// a time, and the batches are derived and validated anyway so that nothing is left to
/// design later.
///
/// **A fixed timestep does not make this deterministic.** What does is the command order, the
/// system order, the commit order, and the numeric rules in docs/DETERMINISM.md. The tick
/// loop only decides how many times to run.
///
/// Thread affinity: main thread, until M8 moves the compute phase.

#include <atlas/core/result.hpp>
#include <atlas/core/time.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/schedule.hpp>
#include <atlas/simulation/world.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace atlas::sim {

/// What one system's writes hashed to after a tick.
///
/// Recorded per tick so that a divergence between two runs can be attributed to a system
/// rather than merely detected. Without this, a mismatched state hash says only that
/// something differs somewhere.
struct SystemHash {
    SystemId system = SystemId::Invalid;
    std::uint64_t hash = 0;
};

/// What a tick did.
struct TickReport {
    Tick tick = 0;
    std::uint64_t state_hash = 0;
    std::size_t commands_applied = 0;
    std::size_t commands_rejected = 0;
    /// One per system that writes anything, in schedule order.
    std::vector<SystemHash> system_hashes;
};

struct KernelConfig {
    /// Seeds every random stream. Part of the replay.
    std::uint64_t seed = 0;

    /// Record a per-system hash each tick.
    ///
    /// On by default: without it a divergence can be detected but not attributed, and the
    /// cost is one hash per written table per tick. A run that has measured the cost and
    /// does not need attribution can turn it off.
    bool record_system_hashes = true;
};

/// Advances a world through ticks.
///
/// Owns neither the world nor the schedule; both outlive it. That keeps the kernel a policy
/// object rather than a container, so a replay can run the same schedule over a different
/// world without rebuilding anything.
class Kernel {
  public:
    Kernel(World& world, Schedule& schedule, CommandQueue& commands, KernelConfig config);

    /// Run exactly one tick.
    ///
    /// Fails only when the setup is wrong, never because of what a system did: a system has
    /// no way to report failure, by design. A command that no longer validates is counted as
    /// rejected and skipped rather than stopping the tick, because one bad command from one
    /// source must not halt a simulation that others are also driving.
    [[nodiscard]] Result<TickReport> step();

    /// Run `count` ticks, returning a report for each.
    [[nodiscard]] Result<std::vector<TickReport>> run(std::uint64_t count);

    /// The tick that `step` will run next.
    [[nodiscard]] Tick current_tick() const noexcept { return m_tick; }

    /// Move to a tick, as a load does. Does not touch the world.
    void set_tick(Tick tick) noexcept { m_tick = tick; }

    [[nodiscard]] std::uint64_t seed() const noexcept { return m_config.seed; }

    void set_seed(std::uint64_t seed) noexcept { m_config.seed = seed; }

    /// Commands that arrived stamped for a tick already run.
    ///
    /// Counted rather than silently dropped: a rising number means something upstream is
    /// late, which is worth knowing before results start depending on it.
    [[nodiscard]] std::uint64_t late_commands() const noexcept { return m_late_commands; }

  private:
    World* m_world;
    Schedule* m_schedule;
    CommandQueue* m_commands;
    KernelConfig m_config;
    Tick m_tick = 0;
    std::uint64_t m_late_commands = 0;
};

}  // namespace atlas::sim
