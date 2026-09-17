# Performance

## Policy

1. No performance claim without a measurement. "Optimised" is not supported by a frame
   rate, and "fast" is not supported by the presence of threads.
2. Measure before optimising: capture a benchmark result or a profiler trace that shows the
   cost. Optimise. Report before and after, and confirm behaviour with tests.
3. Never compare a debug build to a release build, or results from different machines.
4. A benchmark regression triggers a review. Editing the benchmark to make it pass is
   forbidden.
5. Budgets are set only after a baseline exists on the target machine. There are no
   budgets yet, because there is no baseline yet.

## Counters

Rolling counters are collected in the main loop and reported on exit and in the debug UI.
From M1: frame time, tick time, ticks run per frame, dropped ticks. From M2: render submit
time, draw and dispatch counts. From M3: visible objects, upload bytes. From M4: asset
queue depth. From M6: simulation system times, command count, snapshot publication time,
snapshot wait time.

Each counter reports median, 90th percentile, and 99th percentile over a rolling window,
not a mean. Tail behaviour is what makes an application feel unresponsive.

## Profiling workflow

Tracy is integrated from M0 and compiled out unless `ATLAS_PROFILE=ON`. A profiling build
is configured with `cmake --preset macos-debug -DATLAS_PROFILE=ON`, then connected to with
the Tracy GUI. The client and the GUI must be the same version: the network protocol
changes between releases. The pinned pair is recorded in DEPENDENCIES.md.

Zones are added around frame boundaries, acquire, record, submit, tick execution, per
system, and asset load stages. Allocation and lock instrumentation are enabled only when a
specific measurement needs them.

GPU-side timing is not available: SDL_GPU exposes fences but no timestamp queries. GPU cost
is therefore inferred from submit-to-fence latency, and this limitation is stated wherever
GPU timing is reported.

## Benchmark harness

`benchmarks/atlas_bench` appears in M3. It emits machine-readable JSON plus a
human-readable table. Each result records scenario, parameters, sample count, warm-up
policy, median, p90, p99, build configuration, commit, and machine
metadata.

Baselines live in `benchmarks/baselines/<machine-id>.json` and are compared by
`tools/bench_baseline.py`. A baseline is only valid for the hardware, OS, compiler, build
type, and dependency set it records.

## Machine metadata

The owner's development machine, for baseline identity:

| Field | Value |
|---|---|
| Model | Apple M4 Pro |
| Cores | 14 total: 10 performance, 4 efficiency |
| GPU | Apple M4 Pro, 20 cores, Metal 4 |
| Memory | 24 GB unified |
| OS | macOS 26.5.2 |
| Compiler | Apple clang 21.0.0 |

Because the CPU has both performance and efficiency cores, worker-count results are not
linear and every parallel benchmark must record the core split alongside the thread count.

## Scenarios

Provisional and scalable; each is introduced with the subsystem it measures.

| Scenario | Parameters | Introduced |
|---|---|---|
| Data row iteration and mutation | 10k, 100k, 1M rows; structure-of-arrays versus array-of-structures | M3 |
| Draw submission | 10k, 100k quads or instanced cells | M3, extended M7 |
| Asset load throughput | Cold and warm cache, varying payload sizes | M4 |
| Serialization round trip | Scene and simulation state at several sizes | M5, M6 |
| Snapshot publication | Snapshot size versus publication cost | M6 |
| Headless deterministic run | 1k, 10k, 100k ticks | M6, M7 |
| ID picking readback | Latency and stall behaviour | M7 |
| Task overhead | Spawn and join cost, parallel-for granularity | M8 |
| Worker scaling | 1, 2, 4, and hardware concurrency minus one | M8 |

## First results

Recorded on the machine described above, RelWithDebInfo, quad batching only. These are the
processor-side cost of turning quads into an uploaded buffer and a recorded draw; the
graphics processor's own time is not measurable, because SDL_GPU exposes no timestamp
queries, and is therefore not reported.

| Scenario | Median | p90 | p99 |
|---|---|---|---|
| 10k quads submitted | 0.31 ms | 0.78 ms | 3.3 ms |
| 100k quads submitted | 1.45 ms | 2.2 ms | 4.2 ms |
| 10k points transformed | 5.8 us | 5.9 us | 6.3 us |
| Allocations per frame, 1k quads | 8 | | |
| Allocations per frame, 50k quads | 8 | | |

The tail is much worse than the median, by a factor of ten at the smaller size. Two things
turned out to be true about that. Part of it was a real fixed cost, a fence wait, which the
section below removes. The rest is machine noise large enough that the p99 is not a usable
signal here at all: four runs of an unchanged binary spread it by 90%. Tails are still worth
reporting, but on this machine they are worth investigating rather than comparing.

The allocation counts are the evidence for M3's "no unbounded per-frame allocation" exit
criterion. Eight is not zero; what matters is that fifty times the quads does not mean fifty
times the allocations.

**What these numbers are not.** The first version of this benchmark timed a whole frame
including presentation and reported 8.3 milliseconds for every scene size, because that is
the display's refresh interval on this machine. A benchmark that measures the monitor is
worse than no benchmark, because it looks like data. Presentation is now outside the timed
section, and the scales differ as they should.

## Streaming buffer uploads, M7

The first optimisation this project has made, and the first time the measurement policy was
applied end to end.

**What was wrong.** `QuadBatch::flush` updated its instance buffer through
`Device::upload_buffer`, which submits a command buffer and then waits on a graphics fence
before returning. The comment directly above that wait said a non-stalling path "belongs with
the first thing that updates a buffer every frame, and does not exist yet". The batcher was
that first thing, and had used the blocking path since M3.

**How it was measured, and what went wrong with the first attempt.** The plan was to watch the
p99 collapse, on the theory that a tail flat in the payload is a fixed cost. That turned out not
to be measurable here: four runs of the *unchanged* binary gave 10k p99 values from 788
microseconds to 1.49 milliseconds, a spread of 90%. Any effect would have been smaller than the
noise, and the recorded baseline's 4.75 millisecond p99 was outside even that spread, so it had
been recorded under different conditions and was not a usable comparison.

So the wait was measured directly instead, by timing `SDL_WaitForGPUFences` itself across 600
calls: **mean 454 to 505 microseconds, maximum 2.3 milliseconds**. Against medians of roughly
600 microseconds at 10k quads and 1.7 milliseconds at 100k, the wait was about three quarters
of the cost at the smaller size and a quarter at the larger.

**The prediction, written down before the change.** Removing the wait should take the 10k median
to roughly 150 microseconds and the 100k median to roughly 1.25 milliseconds, with no fence
waits from the batcher at all. If the 10k median did not at least halve, the wait was not the
cost and the change would be reverted rather than kept on faith. The p99 was explicitly not
predicted, because it is not a usable signal on this machine.

**The result.**

| Scenario | Before (median) | After (median) | Predicted |
|---|---|---|---|
| 10k quads submitted | 573 to 673 us | 154 to 244 us | ~150 us |
| 100k quads submitted | 1.59 to 1.84 ms | 1.26 to 1.50 ms | ~1.25 ms |

Throughput went from about 15 million to about 41 million quads per second at 10k, and from 59
to 67 million at 100k. The prediction held at both sizes.

**What it costs.** `stream_buffer` cycles the buffer, so a draw already recorded keeps the
contents it was recorded against. That is a guarantee from the graphics library rather than
something Atlas can check, so it has a test: two flushes in one frame drawing two separated
groups in different colours, both required to appear. Disabling cycling makes that test fail
with the first group missing entirely, which was confirmed by doing it.

**The allocation counts are unchanged** at eight per frame for both scene sizes, so the reused
staging buffer did not trade one cost for another.

## The artifact cache, M7: measured and found wanting

The charter lists cooked-artifact caching under v0.1 and M4 deferred it on the grounds that
nothing took long enough to import to justify one. M7 built it and measured it, and the
measurement says the M4 reasoning was right and the M7 design was wrong.

Decoded textures are cached on disk, keyed by a hash of the source bytes plus the importer
version. Content rather than path, so a moved file still hits. The benchmark generates a
noise PNG so the decode has real work to do, and reports the decode, the cache read, and the
cost of computing the key on their own.

| 2048x2048 noise PNG (16 MB) | Median |
|---|---|
| Cold: decode | 8.25 ms |
| Warm: read the cached entry | 2.09 ms |
| Compute the cache key (hash the source) | **15.3 ms** |

The key is paid on every lookup, hits and misses alike, because there is no way to know
whether an entry exists without it. So the warm path costs 15.3 + 2.1 = 17.4 ms against a cold
decode of 8.25 ms. **The cache as keyed makes a texture load slower.** At 512x512 the same holds:
1.02 ms of hashing plus 0.15 ms of reading against a 0.50 ms decode.

The cause is the hash. The canonical hash is FNV-1a a byte at a time, chosen for determinism
and cross-platform stability of *state* hashes, where its speed does not matter. At about one
gigabyte a second it is slower than the PNG decoder it was meant to bypass. Reusing it for
bulk content was a mistake of convenience.

The cache is committed, off by default, correct, and tested, because the infrastructure is
right and only the key strategy is wrong. What replaces the key is an owner decision, recorded
in the roadmap, because the three candidates trade different things: a faster hash is a new
dependency, keying by size and modification time is the industry standard but weaker, and a
first-party word-at-a-time hash is neither.

**Re-keyed.** The owner chose size and modification time, the key every build system uses.
Prediction before the change: the warm path becomes the 2.1 ms read alone, about 4x. Result:

| Noise PNG | Cold decode | Warm read | Computing the key |
|---|---|---|---|
| 512x512, 1 MB | 0.53 ms | 0.14 ms | below timer resolution |
| 2048x2048, 16 MB | 8.46 ms | 2.26 ms | below timer resolution |

3.7x at both sizes, and the key line is now a trivial hash of a path and two integers. The
trade was recorded up front and stands: a file rewritten with the same size inside the
timestamp's resolution serves the previous decode, and a moved file misses. `--cache-dir` is
now safe to use.

The first attempt is left in this history on purpose. A cache that was slower than no cache
passed every functional test; only the benchmark caught it, which is the argument for the
measurement policy in one paragraph.

## The Strategy Lab, M7: a million cells

The lab is the first program to drive the simulation kernel, and the first to draw a million
of anything. Both halves were measured separately, because the milestone's requirement is
that a million cells stay *interactive*, and interactivity is a property of the frame, not of
the tick.

Recorded on the machine described above, RelWithDebInfo, `atlas_bench --filter simulation`,
no device anywhere near the measurement: `atlas::lab_sim` cannot link one.

| Scenario | Parameters | Median | p90 | p99 |
|---|---|---|---|---|
| Kernel tick | 10k cells | 128 us | 130 us | 138 us |
| Kernel tick | 100k cells | 1.30 ms | 1.31 ms | 1.32 ms |
| Kernel tick | 1M cells | 13.4 ms | 13.4 ms | 13.4 ms |
| World hash alone | 1M cells | 11.8 ms | 11.8 ms | 11.8 ms |
| step region value alone | 1M cells | 167 us | 171 us | 172 us |
| accumulate population alone | 1M cells | 1.30 ms | 1.31 ms | 1.31 ms |
| drift owner index alone | 1M cells | 3.7 us | 3.8 us | 3.8 us |
| Deterministic run, 2 commands per tick | 1k, 10k, 100k ticks at 256 cells | 3.6 us per tick, all three |
| Snapshot build | 100k cells | 138 us | 141 us | 143 us |
| Snapshot build | 1M cells | 1.43 ms | 1.44 ms | 1.45 ms |

The attribution is the finding. **The world hash is 88% of a million-cell tick**: one FNV
step per byte over eleven bytes per cell, paid every tick because the tick's contract is
drain, compute, commit, hash. The systems themselves cost 1.5 ms between them, and the two
that use a random stream are negligible because they touch a thousandth of the cells. The
lab first observed 33 ms per tick, not 13, because the kernel was also recording per-system
hashes, which walk the written tables again; those exist to attribute a replay divergence,
so the lab now records them only while recording. Hashing strategy is M8's first question and
`docs/DEFERRED.md` records the two candidate answers.

The frame, measured with the application itself (`atlas_lab`, Release, `--no-overlay`, the
window fitted to the whole grid, frame times from `FrameCounters` over 120 frames):

| Configuration | Median frame | p90 | p99 | Ticks per second achieved |
|---|---|---|---|---|
| 1M cells, paused (draw only) | 8.3 ms | 8.5 ms | 20.8 ms | - |
| 1M cells, 30 tps | 8.9 ms | 21.4 ms | 23.4 ms | 29 (none dropped) |
| 1M cells, 60 tps, catch-up limit 2 | 33.2 ms | 33.6 ms | 35.0 ms | 59 (1 dropped in 120 frames) |
| 1M cells, headless, unbounded | - | - | - | 82 |
| 100k cells, 60 tps | 8.5 ms | 8.8 ms | 9.2 ms | 60 (none dropped) |

Drawing a million cells costs 8.3 ms: 61 flushes of the quad batch, 48 MB of instance data
streamed per frame through the path Gate 1 made non-blocking. That is the requirement met.
At 60 ticks a second the frame is two ticks plus the draw, which is what the catch-up limit
is for: before it, the accumulator's default of eight catch-up ticks produced 270 ms frames.
The limit does not make the simulation faster, it keeps the picture responsive while the
simulation runs at what it can do and reports what it dropped.

**Picking.** A click draws the identifier pass into an `R32Uint` target the size of the
window, one draw per visible chunk with no vertex buffer, and requests a 1x1 readback after
the frame is submitted. The readback is collected **two frames later** on this machine, polled
through a fence; nothing waits on it. Every pick is cross-checked against the analytic inverse
of the projection and a disagreement is logged; none has been observed, including at cell
edges and chunk boundaries in the GPU tests, and through the integration check that
reimplements the chunk-major index formula in Python.

All of these numbers are processor-side. Graphics-processor time is not measurable through
SDL_GPU and is not reported.

## Hashing, M8's first question: measured, not yet changed

M7 found that a million-cell tick spends most of itself hashing. This measures what could be
done about it. Nothing here is adopted: `hash.hpp` says FNV-1a is "not the fastest hash
available" and that `kHashAlgorithmVersion` exists so a faster one can replace it without
silently invalidating stored values, and spending that version number is the owner's decision,
not a performance tweak. `atlas_bench --filter hash` and `--filter simulation`, Release.

**The prediction, written down first.** FNV-1a consumes one byte per multiply and each multiply
waits for the previous one, so the loop should be bound by that dependency chain rather than by
memory: roughly one byte per nanosecond on this machine, which is what 11 MB in 11.2 ms is.
If so, eight bytes per multiply should give close to eight times; independent chains should give
more again; blocking should cost nothing extra; threads should divide what remains. All four
held, which is the only reason the explanation below is worth anything.

### Where the tick's time actually goes

| Part | 1M cells | Share of the tick |
|---|---|---|
| Whole tick | 12.35 ms | |
| `World::hash()` | 10.76 ms | 87% |
| — the `cells` table (7 MB) | 6.99 ms | |
| — the `population` table (4 MB) | 4.09 ms | |
| — the `chunks` table | 2.1 us | |
| — `adjacency` (hash cached at `set`) | below the timer | |
| — `grid` | below the timer | |
| Everything else: four systems, commands, commit | 1.59 ms | 13% |

**This kills the first of the two candidates `DEFERRED.md` recorded.** Hashing only the tables a
tick wrote saves nothing here, because `cells` and `population` are the whole cost and the lab
writes both every tick. The one table where caching would pay, `adjacency`, already caches its
own hash and is free. The idea is not wrong in general; it is worth nothing in this workload,
and this workload is the one that was too slow.

### How fast the hash itself could be

11 MB, the size of the lab's million-cell world.

| Algorithm | Median | Throughput | Avalanche (ideal 32.0) |
|---|---|---|---|
| FNV-1a, one byte per multiply — what the engine does today | 11.20 ms | 1.03 GB/s | 30.67 |
| Eight bytes per multiply | 1.41 ms | 8.2 GB/s | **16.59** |
| Four independent chains, 32 bytes per step | 360 us | 32.0 GB/s | **16.60** |
| Four chains, with a final mix | 360 us | 32.0 GB/s | 32.02 |
| Four chains, mixed, streamed through a 32-byte buffer | 374 us | 30.8 GB/s | 32.02 |
| Blocked into 256 KiB pieces, 2 threads | 217 us | 53 GB/s | 32.11 |
| Blocked, 4 threads | 146 us | 79 GB/s | 32.11 |
| Blocked, 8 threads | 113 us | 102 GB/s | 32.11 |

**The trap is in the avalanche column, and only measuring it found it.** Avalanche is the
average number of the 64 output bits that change when one input bit is flipped; 32 is ideal,
and a hash well below it detects change less reliably, which for a state hash is the entire
job. The obvious fast candidates score 16.6 against FNV-1a's 30.7 — they are twice as fast per
byte and half as good, because a whole word exclusive-ored in passes through exactly one
multiply, and a multiply diffuses upward only, so a flip in a high bit of the last word reaches
almost nothing. FNV-1a avoids this by accident: every byte gets a multiply of its own.

Every avalanche figure above is bit-identical when the same benchmark is built with a
different compiler and standard library, in the Linux container, at a different optimisation
level — which is the portability a canonical hash has to have, obtained here for the price of
running the check. The Linux timings are from a debug build and are not comparable as speed,
so they are not quoted.

The fix is five operations once per hash, whatever the input size: MurmurHash3's fmix64
finaliser. It costs nothing measurable (360 us either way) and lands at 32.02, slightly better
than the hash it would replace. This is a coarse test over one kibibyte, not a statistical
suite; adopting a hash as a compatibility commitment should include a real one.

**Streaming costs nothing either.** `hash.hpp` promises that hashing A and then B gives the same
value as hashing the concatenation, and `Hasher` depends on it: every table adds a row count and
then each column separately. A word-wise hash keeps that promise only by holding partial blocks
across calls, and the buffered version measures 374 us against 360 us, with the same value for
every way of splitting the input, including splits chosen to be awkward. The engine's tables
happen to add spans that are all multiples of eight today, which is the kind of accident that
should not be load-bearing, so the buffer is not optional.

### What this means for the tick, and for M8

Projected, not measured, because measuring it means making the change this measurement exists to
inform: 1.59 ms of work plus 0.37 ms of hashing is **about 2.0 ms against today's 12.35 ms**, a
little over six times faster, from a sequential change with no threading at all.

**That kills the second candidate too.** Threads take the hash from 374 us to 113 us, which
sounds like a lot until it is put back in the tick: 0.26 ms off a 2.0 ms tick, about 13%, in
exchange for a worker pool, a dispatch policy, and a parallel correctness argument. The
sequential change gets 96% of the available win. Parallel hashing is not what M8 should build
first; it may not be what M8 should build at all.

Run-to-run variance on these figures is about 6% on this machine, which is smaller than every
difference the conclusions rest on.

### The decision this leaves

Adopting any of these means `kHashAlgorithmVersion` becomes 2, and every stored hash computed
under version 1 stops matching: saves, replay checkpoints, artifact cache entries, and the
golden-scenario constants in both test suites. None of that is silent — the version is written
alongside the values and checked on read, which is exactly the situation it was put there for —
but it is a change to a stored format, so it is the owner's to make, along with whether
`hash_string`, which computes table, system, command and stream identifiers at compile time and
needs no speed at all, changes with it or stays as it is.

## Optimisation candidates

Recorded as hypotheses, not commitments. Each requires a trace before it is attempted:
batching draw submissions, frustum and chunk culling, compact data layouts, dirty-range
snapshot publication, cached derived data, a faster state-hash algorithm, and reduced
synchronisation in the commit phase.
