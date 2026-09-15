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

## Command ordering

Commands are stamped with a target tick, a source identifier, and a monotonic per-source
sequence number. At the start of a tick, the commands targeting it are drained and sorted
by `(source, sequence)`, which is a total order independent of arrival time. Payloads are
validated by a registered decoder before they are applied; a rejected command is logged and
dropped rather than partially applied.

## System ordering

Systems have stable identifiers derived from hashed names, and an explicit order in the
schedule, validated for duplicates. Each system declares its read and write sets at table
granularity. The schedule derives conflict-free batches from those declarations.

The compute phase writes only to system-private scratch buffers. The commit phase applies
those buffers in system order, and within a buffer in stable index order. No result may
depend on which worker finished first: reductions merge per-worker partials in
worker-index order.

Until M8 the compute phase runs sequentially. The batches are still computed and validated,
so parallel execution in M8 is a scheduling change rather than a redesign.

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
  throughput without silently invalidating stored hashes.

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

From M6: replay the same command log several times in one process and across processes,
comparing per-tick hashes. From M8: repeat across worker counts 1, 2, 4, and hardware
concurrency minus one. Golden hashes for a small fixed scenario are also compared between
macOS arm64 and Linux x86_64 in CI to measure, and document, cross-architecture behaviour.
