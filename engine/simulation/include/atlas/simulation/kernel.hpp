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
/// Compute and commit are separate because that is what lets compute run on worker threads
/// without changing a result. M8 did exactly that: a batch with more than one system is
/// dispatched across the pool, and the state hash is identical at every worker count.
///
/// **A fixed timestep does not make this deterministic.** What does is the command order, the
/// system order, the commit order, and the numeric rules in docs/DETERMINISM.md. The tick
/// loop only decides how many times to run.
///
/// Thread affinity: main thread. Since M8 the compute phase may run on `tasks` workers when
/// a pool is supplied, which the kernel owns the dispatch of; commit and hash stay here.

#include <atlas/core/result.hpp>
#include <atlas/core/time.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/schedule.hpp>
#include <atlas/simulation/turn_gate.hpp>
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

    /// Commands stamped for a tick that had already run when this tick drained them.
    ///
    /// Split from the invalid ones in M14, because under lockstep the two mean opposite things.
    /// A late command is a protocol violation: a peer has fallen further behind than the turn
    /// delay allows, or is claiming a tick it cannot have, and either way the session is no
    /// longer sound. An invalid command is one source sending bad bytes, which is ordinary and
    /// local and which every peer rejects identically. Counted together, a monitor cannot tell
    /// a broken network from a broken mod — and before M14 only the cumulative
    /// `Kernel::late_commands()` could tell them apart at all.
    std::size_t commands_late = 0;

    /// Commands whose payload no longer validated, or whose type has lost its handler.
    std::size_t commands_invalid = 0;

    /// Late plus invalid: what `commands_rejected` meant before M14, unchanged.
    ///
    /// A function rather than a third field so the three cannot drift. A field would have to be
    /// incremented in both branches, and forgetting one is a mistake no test that checks the
    /// split would notice.
    [[nodiscard]] std::size_t commands_rejected() const noexcept {
        return commands_late + commands_invalid;
    }

    /// One per system that writes anything, in schedule order.
    std::vector<SystemHash> system_hashes;

    /// The commands that were applied, in the order they were applied.
    ///
    /// Populated only when `KernelConfig::record_applied_commands` is set, because copying
    /// every payload each tick costs something a run that is not being recorded should not
    /// pay. Rejected commands are not included: a recording is of what happened, and a
    /// command that was refused did not happen.
    std::vector<Command> applied_commands;
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

    /// Copy the applied commands into each tick's report, for recording a replay.
    ///
    /// Off by default. A run that is not being recorded should not pay to copy payloads it
    /// will never look at.
    bool record_applied_commands = false;

    /// Optional, and borrowed: the kernel does not own it and it must outlive the kernel.
    ///
    /// With a pool, the systems of one batch run concurrently and each system may split its own
    /// work further. Without one, everything runs on the calling thread. The results are the
    /// same either way, which the determinism tests check rather than assume.
    tasks::WorkerPool* pool = nullptr;

    /// Which ticks are allowed to run yet. Optional, and borrowed: the kernel does not own it
    /// and it must outlive the kernel. It only ever asks; marking a turn is the driver's job,
    /// which is why this is const.
    ///
    /// Null is a solo run and is the default, and a solo run takes byte-for-byte the path it
    /// took before M14 — `step` cannot refuse, `ready` is always true, and the golden hashes
    /// are the golden hashes. A gate expecting nobody behaves identically; the null case exists
    /// so a caller with no notion of peers need not construct one.
    const TurnGate* gate = nullptr;
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
    /// Fails when the setup is wrong — a system has no way to report failure, by design, and a
    /// command that no longer validates is counted and skipped rather than stopping the tick,
    /// because one bad command from one source must not halt a simulation that others are also
    /// driving.
    ///
    /// **And fails with `Unavailable` when a turn gate says this tick is not ready yet**, naming
    /// the sources it is waiting on. That is a runtime condition rather than a setup error, and
    /// it is the one place this contract was widened: until M14 this sentence read "fails only
    /// when the setup is wrong, never because of what a system did". The change is deliberate
    /// and is recorded in [ADR-0014](../../../../../docs/adr/0014-deterministic-lockstep.md)
    /// rather than left to be discovered here.
    ///
    /// **A refused tick changes nothing**: no command is drained, no system runs, the tick
    /// number does not move, and retrying it later is exactly equivalent to having waited. That
    /// holds by construction rather than by cleanup, because the check precedes every mutation
    /// — in particular it precedes the drain, which *removes* what it returns.
    ///
    /// **Ask `ready()` first.** Constructing an `Error` allocates, and a stall lasting a
    /// thousand frames should not allocate a thousand formatted strings inside the tick loop to
    /// report that nothing happened. A refusal is for a caller that did not ask.
    [[nodiscard]] Result<TickReport> step();

    /// Whether `step` would run this tick rather than refuse it.
    ///
    /// Answers the gate's question and only the gate's question: a schedule that was never
    /// finalised still fails in `step`, deliberately, because that is a setup error asked once
    /// at startup while this is asked every frame.
    [[nodiscard]] bool ready() const noexcept;

    /// The first tick `step` would refuse.
    ///
    /// `current_tick()` when it would refuse now, and `TurnGate::kUnboundedHorizon` with no
    /// gate. Half-open, so `ready_horizon() - current_tick()` is how many ticks may run from
    /// here with no adjustment.
    [[nodiscard]] Tick ready_horizon() const noexcept;

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
    /// late, which is worth knowing before results start depending on it. Under lockstep it is
    /// worse than that — a late turn is a protocol violation and the session ends — which is
    /// why the per-tick report separates this from an invalid command as well.
    [[nodiscard]] std::uint64_t late_commands() const noexcept { return m_late_commands; }

    /// Commands refused by their own validator at the tick they named.
    ///
    /// The other half of what a rejection can mean. One source sending bad bytes is ordinary;
    /// every peer refuses it identically, and the run is unaffected.
    [[nodiscard]] std::uint64_t invalid_commands() const noexcept { return m_invalid_commands; }

    /// Calls to `step` refused because the gate was not ready.
    ///
    /// Counted apart from the accumulator's `dropped_ticks`, and **the two must never be added
    /// together**. A dropped tick is one the simulation decided not to run and never will; a
    /// stalled tick is one it will run, at its own number, as soon as the marks arrive. Summing
    /// them would report a stall as lost work and send somebody optimising a simulation that
    /// was waiting on a peer.
    ///
    /// Attempts rather than ticks: one tick refused on three hundred consecutive frames counts
    /// three hundred, which is why it is named for steps and not for ticks.
    [[nodiscard]] std::uint64_t stalled_steps() const noexcept { return m_stalled_steps; }

  private:
    World* m_world;
    Schedule* m_schedule;
    CommandQueue* m_commands;
    KernelConfig m_config;
    Tick m_tick = 0;
    std::uint64_t m_late_commands = 0;
    std::uint64_t m_invalid_commands = 0;
    std::uint64_t m_stalled_steps = 0;
    /// Reused by the refusal message, so a stall asked about every frame does not allocate one.
    std::vector<SourceId> m_waiting;
};

}  // namespace atlas::sim
