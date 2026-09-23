# Determinism

## What Atlas claims

**Replay determinism, for the same build on the same platform.** Replaying a command log
against the same initial state reproduces the same per-tick state hashes.

Atlas does **not** claim bit-identical simulation across compilers, standard libraries, or
CPU architectures. That would have to be proven, not asserted. Cross-platform hashes are
compared in CI as a *measurement*: divergence between macOS arm64 and Linux x86_64 is
recorded as a known limit, not treated as a build failure, unless the numeric policy says
the code path should have been architecture-independent.

Using a fixed timestep does not make a simulation deterministic. The guarantees below come
from ordering, hashing, and numeric policy, not from the tick loop.

## Tick model

Simulation state advances only in whole ticks. `Tick` is a 64-bit unsigned counter; tick
length is configured, defaulting to 50 ms in the laboratory applications. Wall time never
enters simulation APIs: a tick context carries a `Tick`, never a duration.

The accumulator that decides *how many* ticks to run is integer-only and takes no clock of
its own. Speed is a rational multiplier applied when converting elapsed real time into
accumulated tick units; it never scales any value inside a simulation equation. Catch-up is
clamped and surplus ticks are discarded, so falling behind changes throughput, never results.

Under a turn gate that last sentence needs a second half rather than a correction, because it
stays exactly true of every ungated run — which is every solo run, for ever. A gated tick is
neither run nor discarded but **postponed**: it runs at its own number as soon as every source
has reported. So a slow peer changes *when* a tick runs and never *which commands it runs*, and
a stall is counted separately from a dropped tick precisely because the two are opposite things.
See [ADR-0014](adr/0014-deterministic-lockstep.md).

**A real socket changes none of that, and M17 is the milestone that proved it rather than
assumed it.** Two processes on two ends of a network interface reach the same hash at the same
tick, because the transport only ever decides *when* a turn arrives and never *whether* it is
applied: a turn is stamped with the tick it belongs to before it is sent, and a turn that
arrives late is a protocol violation rather than a command applied at the wrong moment.

**The one deadline in the design lives in the transport and not in the gate.** A peer that has
gone quiet for long enough ends the session ([ADR-0017](adr/0017-lockstep-transport.md) D5), and
the gate never learns that a clock exists — readiness still depends only on who has reported.
Dropping a quiet peer and playing on would be the decision that broke this, because it is
simulation-visible: two peers applying the same drop at different ticks compute different
states, which is why it is deferred rather than defaulted.

**A session between different builds is permitted only when both peers produce the same golden
hashes**, exchanged at the handshake and refused on a mismatch. That is a probe rather than a
proof: two builds agreeing about the fixed scenario agree about the simulation as far as anything
here can tell, which is the same standard the cross-platform table below is held to. A differing
build identifier is logged and not refused, so a debug peer and a release peer whose goldens
agree can still play.

## Command ordering

Commands are stamped with a target tick, a source identifier, and a monotonic per-source
sequence number. At the start of a tick, the commands targeting it are drained and sorted
by `(source, sequence)`, which is a total order independent of arrival time. Payloads are
validated by a registered decoder before they are applied; a rejected command is logged and
dropped rather than partially applied. A command that is well-formed and on time may still be
**declined** by its handler on world state — the cell is owned, it is not this source's turn
(ADR-0019). A decline depends only on the world and the payload, which every peer holds
identically at that tick, so every peer declines identically; it is counted on its own, changes
nothing, and is not recorded, because a recording is of what changed the state.

## System ordering

Systems have stable identifiers derived from hashed names, and an explicit order in the
schedule, validated for duplicates. Each system declares its read and write sets at table
granularity. The schedule derives conflict-free batches from those declarations.

The compute phase writes only to system-private scratch buffers. The commit phase applies
those buffers in system order, and within a buffer in stable index order. No result may
depend on which worker finished first: reductions merge per-worker partials in
worker-index order.

Until M8 the compute phase ran sequentially. Because the batches were computed and validated
from the start, making them parallel was a scheduling change rather than a redesign: the kernel
dispatches a batch of more than one system across an `atlas::tasks` worker pool, and each
system's rows are split by the partitioning rule above.

## Random numbers

A counter-based generator keyed by `(seed, stream_id, tick, counter)`, where `stream_id` is
the hash of a stream name. Streams are opened by name and are reproducible independently of
how many values other streams consumed. There is no ambient or global RNG, and no
generator carries hidden mutable state across ticks.

## State hashing

A canonical 64-bit hash over authoritative tables in a fixed order, with per-system
sub-hashes so a divergence can be attributed. Hashing rules:

- Explicit-width integers only; no `size_t`, no `long`, no `long double`.
- Floating-point values are hashed by bit pattern, never by formatted text.
- Tables are hashed in index order; associative structures are hashed through a sorted
  index array, never by iterating the container.
- The hash algorithm's identity is versioned in the header, so it can be replaced for
  throughput without silently invalidating stored hashes. It was, in M8: version 2 keeps FNV-1a
  for `hash_string`, which computes table, system, command and stream identifiers at compile
  time, and uses a four-lane block hash for bulk content, which is thirty times faster and, by
  avalanche, slightly better at detecting change. Values from version 1 do not match version 2,
  and are refused by the version check rather than misread.
- A parallel loop divides its range into chunks that depend only on the count and the grain,
  never on how many workers there are. Which worker runs which chunk varies; what each chunk
  computes does not, so a body that writes only its own indices produces the same bytes at any
  worker count. That is how M8's worker-count invariance is obtained: by making the work
  identical rather than by making the schedule reproducible.
- The bulk hash consumes thirty-two bytes at a time and holds partial blocks, so its value
  depends on the bytes hashed and not on how a caller divided them between calls. It cannot be
  continued from a previous value the way FNV-1a could, and the seed parameters that implied
  otherwise were removed rather than left to mean something subtly different.

Per-tick hashes are logged during replay runs. On divergence, the first differing tick and
the first differing system sub-hash are reported.

## Numeric rules

- `-ffp-contract=off` on Clang and GCC, `/fp:precise` on MSVC, from milestone M0. Without
  this, arm64 contracts multiply-add into FMA while x86_64 without FMA enabled does not,
  which changes results.
- `-ffast-math` and equivalents are forbidden everywhere.
- `long double` is forbidden: its width differs across platforms.
- Authoritative simulation state is integer or fixed-point by default. Floating point in
  authoritative state requires a recorded justification and a passing cross-architecture
  golden-hash comparison.
- Transcendental functions from libm are forbidden in authoritative simulation code: their
  results are not specified to the last bit and differ between platforms.
- Float-to-integer conversion is only valid in range. Out-of-range conversion saturates on
  arm64 and yields an indefinite value on x86_64, so UBSan's `float-cast-overflow` check is
  enabled in sanitizer builds.
- Presentation code is free to use floating point: it never feeds back into state.

## Replay format

A replay contains the initial-state identity, seed and stream metadata, the tick-stamped
command log, version metadata for the engine build and schemas, and periodic state hashes.
Replaying loads the initial state, feeds commands at their target ticks, and compares
hashes at the recorded checkpoints.

## Save format

Save files begin with a magic value and a format version, then engine build identity,
schema versions, integrity information, and the authoritative state needed to resume.
Authoritative state is written in canonical order. Loading validates counts, lengths,
versions, and bounds before any allocation or indexing, because a save file is untrusted
input. Migration support is written when the first real schema change happens, not before.

## Mods

A mod is a command source and never a system: it runs outside the tick, reaches state only by
submitting, and cannot execute during one ([ADR-0009](adr/0009-scripting-decision.md) decision 2,
[ADR-0015](adr/0015-sandboxed-mods.md)).

Under replay that is all the protection needed — a mod's output is commands, commands are
recorded, and a recording made with a mod plays back on a build that never loads one.

**Under lockstep it is not**, and this is the part M14 changed. Every peer runs the same mods and
each produces its own commands without sending them, so a mod must reach the same decision on
every machine or the peers diverge in their *inputs*. That is why the guest interface has no
clock, no host generator and no floating-point host calls, why `atlas_random` is keyed by seed,
tick and the mod's identity, and why WebAssembly was chosen over a scripting language whose
`math.sin` is the host's libm — which the rule above forbids in authoritative code.

The instruction budget is counted in **instructions and never in wall time**, for the same
reason: a wall-clock watchdog fires after a different amount of work on a fast machine than on a
slow one, so two peers would disable a mod at different ticks. That would be a safety mechanism
causing the failure it exists to prevent.

## Known sources of nondeterminism

Each is either eliminated by the rules above or explicitly out of authoritative state:

| Source | Handling |
|---|---|
| Floating-point contraction and fast-math | Disabled by compiler flags |
| libm transcendentals | Forbidden in authoritative code |
| Float-to-int out of range | UBSan-checked; range is a precondition |
| `std::unordered_*` iteration order | Iteration where order is observable is forbidden |
| Pointer and address values | Never hashed or serialized; stable IDs instead |
| Clocks and wall time | Never enter simulation APIs |
| Filesystem enumeration order | Directory listings are sorted before use |
| Thread completion order | Reductions merge in worker-index order; commit is sequential |
| Data races | TSan configuration; simulation workers write only private buffers |
| Uninitialised memory | MSan is not used; initialisation is enforced by review and ASan |
| Container capacity and iteration of hash maps | Not part of state; never serialized |
| Compiler, standard library, and architecture differences | Not claimed; measured and recorded |

## How determinism is tested

Replaying the same command log reproduces the same per-tick hashes, checked repeatedly in one
process and across a write to bytes and a read back. A recording that is altered must be
caught: the suite corrupts a checkpoint and requires the playback to report the tick and name
the first system whose writes differ. It also changes a command payload and changes the seed,
and requires each to diverge, because a playback that silently ignored its own log would pass
every other test.

Submission order is tested directly: the same commands fed in the reverse order must produce
the same state, since they are ordered by source and sequence rather than by arrival.

Save, load and replay are checked against each other rather than only on their own. A run
saved midway and resumed from the file must reach the hash the uninterrupted run reached.

From M8: the same, repeated across worker counts 1, 2, 4, and hardware concurrency minus one.

## Measured, as of 2026-09-17

A fixed integer-only scenario of 500 ticks over 64 rows with three systems and a random
stream produces these values:

| Quantity | Value | Hash version |
|---|---|---|
| Final state hash | `0xAA82430DE2321AFF` | 2 |
| Hash over all 500 tick hashes | `0x2603546C5687E95E` | 2 |
| Final state hash | `0xCECE73AEEC22FBCA` | 1, superseded |
| Hash over all 500 tick hashes | `0xD71CEC7C1078DD46` | 1, superseded |

**The version 1 rows are kept so that a hash from an old save or an old log can be recognised
rather than puzzled over.** They are not a second opinion about the same computation: M8
replaced the hash algorithm, `kHashAlgorithmVersion` became 2, and every stored hash changed
with it. The simulation's state did not: the M8 change was verified by restoring the old
algorithm and confirming it still produced the version 1 values, which is what distinguishes a
new hash of the same state from a changed state.

**These values are identical across two architectures, two compilers and two standard
libraries:**

| Platform | Compiler | Standard library | Where |
|---|---|---|---|
| macOS arm64 | Apple Clang 21 | libc++ | development machine |
| Linux arm64 | Clang 19 | libstdc++ | local container |
| Linux x86_64 | Clang 19 | libstdc++ | continuous integration |
| Windows x64 | MSVC | Microsoft STL | continuous integration |

The test carrying the values, `engine/simulation/tests/test_golden.cpp`, is compiled and run in
all four; a mismatch anywhere fails that job. The x86_64 result arrived on 2026-09-15, when
continuous integration ran for the first time; before that this section said the comparison had
not been made, because it had not. The version 2 values have been green on every platform since
they were recorded on 2026-09-16.

**arm64 and x86_64 agreeing is the result worth having.** It is the pair the numeric policy
was written for: arm64 contracts multiply-add into a fused instruction by default and x86_64
without those instructions does not, which is why `-ffp-contract=off` has been set since M0.

What this does **not** establish:

- **Windows and MSVC are unmeasured.** A different compiler with a different standard library
  and a different optimiser. Nothing here predicts it.
- **The scenario uses no floating point.** It is integer-only on purpose, so agreement across
  architectures is expected rather than surprising. A scenario with floating point in
  authoritative state would be the interesting case, and there is not one, because
  [ADR-0008](adr/0008-numeric-and-save-policy.md) requires a recorded justification before
  there can be.

So the honest reading is that the ordering rules, the hashing, and the random mixer are
architecture-independent, and that the hard question about floating point has not been asked
yet because nothing has needed to ask it.
