# 0003 — Simulation and render separation

## Status

Proposed, 2026-09-14. Becomes Accepted when M6 implements it.

## Context

The target workload is a world simulation that must stay responsive while simulated time
advances quickly, including a mode that runs as fast as the machine permits. Two properties
are in tension: the simulation must be authoritative, deterministic, and reproducible, while
the interface must stay smooth regardless of how expensive a tick is.

The naive design couples them: advance the world by the frame delta and draw the live state.
That makes results depend on frame rate, makes replay impossible, and makes any future
threading a retrofit against shared mutable state.

## Decision

**Three separate notions of time.** Real time drives responsiveness and frame pacing.
Render time, expressed as seconds since start and an interpolation factor, drives visuals.
Integer ticks drive authoritative state. Wall time never enters a simulation API: a tick
context carries a tick number, never a duration.

**Fixed integer ticks.** Simulation advances only in whole ticks of configured length. How
many ticks run in a given frame is decided by an integer accumulator that takes no clock of
its own, which is what makes it directly unit-testable.

**Speed is a scheduler policy, not a multiplier.** The policies are `Paused`, `SingleStep`,
`Realtime` with a rational rate, and `Unbounded`. No speed setting ever scales a value
inside a simulation equation; it only changes how many ticks are requested.

**Bounded catch-up.** Interactive policies clamp the ticks executed per frame and report how
far behind the simulation fell. Surplus ticks are discarded rather than retained, which is
what prevents the spiral where each frame's catch-up work makes the next frame longer.
Falling behind changes throughput, never results.

**Immutable presentation snapshots.** The simulation owns authoritative state. After a tick
it publishes a snapshot containing only presentation-relevant, plainly-copyable,
stably-ordered data, as a `shared_ptr<const Snapshot>` with latest-wins semantics. The
renderer pins one snapshot for an entire frame and may interpolate between two of them. The
renderer never reads authoritative state and never mutates anything the simulation owns.

**Single-threaded first.** v0.1 runs simulation on the main thread. No render thread is
created until a trace proves the need and thread-affinity rules are written down. The
snapshot channel exists so that moving the simulation to its own thread later is a change of
where a function is called, not a redesign.

## Alternatives

**Variable timestep driven by frame delta.** Simpler, and what most small engines do. It
makes results depend on frame rate, which destroys replay, save-and-resume equivalence, and
any future lockstep multiplayer. Rejected outright.

**Shared mutable state with locks.** Let the renderer read live state under a reader-writer
lock. Avoids snapshot copies, but couples frame rate to tick duration through contention,
makes tail latency unpredictable, and converts every future parallel system into a locking
problem. Rejected.

**Double-buffered state rather than published snapshots.** Two full copies of authoritative
state, swapped each tick. Bounded memory and no allocation, but it forces the renderer to
understand authoritative layout, which is precisely the coupling being avoided, and it
copies far more than presentation needs. Rejected; the snapshot is a projection, not a copy.

**A render thread from the start.** Better parallelism on paper. In practice it multiplies
debugging surface across every subsequent milestone before any measurement shows it is
needed. Rejected for v0.1, revisitable with trace evidence.

## Consequences

- Replay, save and resume, and headless acceleration all follow naturally, because nothing
  authoritative depends on rendering.
- The renderer can be slow, skipped, or absent without affecting simulation results, which
  is what makes headless CI and unbounded mode work.
- Snapshot publication has a real cost that grows with world size. It is benchmarked from
  M6, and dirty-range publication is the first optimisation if measurement justifies it.
- Interpolation requires keeping the previous snapshot as well as the current one.
- Presentation-only state must be identified explicitly. Anything the renderer needs has to
  be projected into the snapshot, which is friction by design: it makes coupling visible.

## Rollback cost

Low in code, high in consequence. This is a design constraint enforced by module boundaries
rather than a large body of code. Abandoning it would mean giving up replay, deterministic
save and resume, and any credible path to multiplayer, so the realistic cost is not
rewriting the loop but losing the properties the project exists to have.
