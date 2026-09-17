# Performance

## Policy

1. No performance claim without a measurement. "Optimised" is not supported by a frame
   rate, and "fast" is not supported by the presence of threads.
2. Measure before optimising: capture a benchmark result or a profiler trace that shows the
   cost. Optimise. Report before and after, and confirm behaviour with tests.
3. Never compare a debug build to a release build, or results from different machines.
4. A benchmark regression triggers a review. Editing the benchmark to make it pass is
   forbidden.
5. Budgets are set only after a baseline exists on the target machine. Since M8 there is a
   baseline and a threshold: a scenario more than 1.25x slower than the recorded baseline
   fails `tools/bench_baseline.py compare`. See "Regression thresholds, M8".

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
so the lab now records them only while recording. Hashing strategy became M8's first change;
both candidates recorded here were killed by measurement, and the answer turned out to be a
faster algorithm rather than hashing less. See "Hashing, M8's first change" below.

The frame, measured with the application itself (`atlas_lab`, Release, `--no-overlay`, the
window fitted to the whole grid, frame times from `FrameCounters` over 120 frames):

**These frame times were misread, and the correction is below the table.** They are the
display's, not the engine's.

| Configuration | Median frame | p90 | p99 | Ticks per second achieved |
|---|---|---|---|---|
| 1M cells, paused (draw only) | 8.3 ms | 8.5 ms | 20.8 ms | - |
| 1M cells, 30 tps | 8.9 ms | 21.4 ms | 23.4 ms | 29 (none dropped) |
| 1M cells, 60 tps, catch-up limit 2 | 33.2 ms | 33.6 ms | 35.0 ms | 59 (1 dropped in 120 frames) |
| 1M cells, headless, unbounded | - | - | - | 82 |
| 100k cells, 60 tps | 8.5 ms | 8.8 ms | 9.2 ms | 60 (none dropped) |

**Correction, M8.** "Drawing a million cells costs 8.3 ms" was wrong. 8.3 ms is 120 Hz, and
the same figure comes back from a four-thousand-cell grid: the frame was waiting for the
display, not for the engine. The lab's frame timer measured wall time between frames and its
"draw" phase counter enclosed acquiring a swapchain image, so both reported the refresh interval
whenever the engine was faster than it. `bench_quads` carries a comment about exactly this trap;
the lab walked into it anyway. What made it hard to notice is that at a million cells the true
cost happens to sit near the refresh interval, so the number looked plausible at the one size
anybody checked.

Measured properly by `atlas_bench --filter cell_field`, which excludes acquiring and presenting
and times only culling, filling instances from the snapshot, and recording the draws:

| Cells | Median | p90 | p99 | Throughput |
|---|---|---|---|---|
| 10k | 93 us | 115 us | 219 us | 108 M cells/s |
| 100k | 708 us | 742 us | 862 us | 145 M cells/s |
| 1M | 7.11 ms | 7.39 ms | 7.55 ms | 148 M cells/s |

Linear in the cell count, at about 6.8 nanoseconds each, which is what a copy-bound path looks
like: every drawn cell is written three times before it reaches the graphics processor — into a
scratch quad, into the batch's instance array, and into the transfer buffer — at 48 bytes a
time, so a million cells move about 144 MB per frame. The lab's overlay now reports drawing and
acquire-and-present as separate phases, so the display's wait cannot be read as the engine's
work again.
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

## Hashing, M8's first change: measured, then made

M7 found that a million-cell tick spends most of itself hashing. This is what was measured,
what it ruled out, and what was adopted. `hash.hpp` said FNV-1a was "not the fastest hash
available" and that `kHashAlgorithmVersion` existed so a faster one could replace it without
silently invalidating stored values; that version number has now been spent, on the owner's
decision, and is 2. `atlas_bench --filter hash` and `--filter simulation`, Release.

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

Every avalanche figure above is bit-identical on AppleClang and arm64, on clang with
libstdc++, and on MSVC 19.51 and x86_64 — three compilers, two architectures, three standard
libraries, four optimisation levels. That is the portability a canonical hash has to have, and
since M8 it is checked on every push rather than asserted: continuous integration builds the
benchmarks on all three platforms and runs this group, whose own checks refuse to report a
number for a candidate that is not deterministic or that ignores part of its input.

The *speed* is not portable in the same way and the table above is one machine's. The same
comparison on a shared two-core Windows runner puts FNV-1a at 15.3 ms and the four-lane
candidate at 2.1 ms, a ratio of about seven rather than thirty-one. Runner timings are noise
and nothing asserts a threshold on them; the point is only that the conclusion does not depend
on the ratio, because even the pessimistic figure moves the hash from most of the tick to a
small part of it.

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

### After

Adopted: the four-lane hash with the final mix, for `hash_bytes` and `Hasher` only.
`hash_string` keeps FNV-1a, because its values are compile-time identifiers written into saved
files and gain nothing from speed. `kHashAlgorithmVersion` is 2 and names the pair.

`tools/bench_baseline.py compare` against the baseline recorded under version 1, same machine:

| Scenario | Before | After | |
|---|---|---|---|
| `simulation/hash`, 1M cells | 11.57 ms | 0.37 ms | 31x |
| `simulation/tick`, 1M cells | 13.05 ms | 2.00 ms | 6.5x |
| `simulation/tick`, 100k cells | 1.28 ms | 0.23 ms | 5.5x |
| `simulation/tick`, 10k cells | 0.128 ms | 0.024 ms | 5.3x |
| `simulation/run`, 100k ticks | 351 ms | 85 ms | 4.1x |
| `simulation/system/*`, 1M cells | unchanged | unchanged | 0.98-1.00x |
| `simulation/snapshot`, 1M cells | unchanged | unchanged | 0.99x |

The projection written above before any code was changed was "about 2.0 ms". The measurement is
2.001 ms. The systems and the snapshot are unchanged to within noise, which is the check that
nothing else moved.

End to end, `atlas_lab` at a million cells and 60 ticks per second: median frame 33.2 ms to
**10.7 ms**, and headless throughput 82 to **534 ticks per second**.

Two consequences worth recording. The lab's `--max-ticks-per-frame` default went back from 2 to
the tick scheduler's own 8: the low limit existed because a tick cost tens of milliseconds, and
at 2 ms it only prevents catching up after a slow frame — measured at 150 frames, a limit of 2
drops five ticks with a 32.6 ms p99 where 4 and 8 drop none with a 17 ms p99. And the frame is
now dominated by drawing. That claim rested on a misread frame timer and the corrected figure
is above: submitting a million cells costs 7.11 ms of processor time, which is still the largest
single cost in the frame, so instance compaction rather than hashing remains the next thing
worth measuring.

### What it cost

Every stored hash computed under version 1 stopped matching, which the version check makes loud
rather than silent: saves and replays are refused with a version mismatch, and artifact cache
keys change because `key_for` hashes `kHashAlgorithmVersion` into the key, so old entries become
unreachable rather than misread. Both golden-scenario tests were re-recorded, and the question
the rules require answering — whether the change was meant to alter simulation results — was
answered by experiment rather than assertion: with the old hash restored and everything else as
it is now, both scenarios still produced their version 1 constants exactly, so the simulation's
state is bit-identical and only the function that reduces it to a number changed.

### What was ruled out

Both candidates the roadmap expected to help: hashing only the tables a tick wrote, which saves
nothing because the tables it writes are the whole cost, and parallel hashing, which would take
the hash from 374 us to 113 us at the price of a worker pool — 13% of a 2.0 ms tick for a
parallel correctness argument, when the sequential change was already worth 31 times.

Still true, and still the honest limit of this work: the avalanche test is coarse, one kibibyte
and one property. A statistical suite would be better evidence than it, and the fact that the
adopted hash scores 32.0 where the one it replaced scored 30.7 is a reason to think the change
is safe, not a proof of it.

## Instance compaction, M8: four bytes a cell

The correction above left the cell field as the largest single cost in the frame: 7.11 ms of
processor time to submit a million cells, linear at about 6.8 nanoseconds each, with every drawn
cell written three times at forty-eight bytes before it reached the graphics processor.

**The prediction, written down first.** Forty-eight bytes an instance is right for a sprite,
which can be anywhere, any size, showing any part of any texture. A grid cell can be none of
those: its rectangle follows from its index, because cells are stored chunk-major and a run of
consecutive chunks is a contiguous range of them. Sending only the colour would take the
per-cell traffic from about 144 bytes to about 8. If the path is bandwidth-bound that is 7.11 ms
to roughly 0.4 ms; allowing for loop and submission overhead, the prediction was **0.8 to
1.5 ms**.

**The prediction was wrong twice, in opposite directions, and that is the useful part.**

Compaction alone gave 2.23 ms — a 3.2x gain where 5 to 9 was predicted. Traffic had fallen
eighteen-fold and time only threefold, so the path was no longer bandwidth-bound and something
else had become the limit. It was `std::vector::push_back`: a capacity check and a size
increment per cell, which had been invisible while each cell also moved 144 bytes. Writing
through a pointer into storage sized once by `set_layout` removed it.

| Cells | Before | Compacted | And without push_back | |
|---|---|---|---|---|
| 10k | 93 us | 55 us | **27 us** | 3.4x |
| 100k | 708 us | 246 us | **67 us** | 10.6x |
| 1M | 7.11 ms | 2.23 ms | **303 us** | **23.5x** |

At a million cells that is 0.29 nanoseconds a cell and about 28 GB/s of traffic, which is what
bandwidth-bound actually looks like on this machine. The final figure is better than the
original prediction, which had been made for the wrong reason: the estimate of the traffic was
about right, and the estimate of what else the loop was doing was absent.

**What it cost elsewhere.** The instance buffer is 4 MB at a million cells rather than 48 MB,
and the 48 MB of resident per-cell geometry is gone entirely — `set_layout` is now one
allocation and no per-cell work, where it used to build a rectangle for every cell. The shader
derives each cell's rectangle from its instance index and a per-run uniform, which is the
identifier pass's technique applied to the picture: the second call site, which is what
justified having it at all rather than a third shader repeating it.

**The picture is unchanged**, verified by rendering the same seed before and after and comparing
every byte: 17% of channel values differ and every one of them by exactly one of 255. That is
the quantisation moving, not the geometry — the old path sent a float colour that the graphics
processor rounded when writing an eight-bit target, and the new one rounds before upload. A
geometry error would not look like that.

## Snapshot pooling, M8: measured and not built

`docs/DEFERRED.md` set the condition for building a pool: the counter had to say that the
allocation, rather than the fill, was what cost. It does not.

| Cells | Filling a reused snapshot | Allocating a fresh one | Difference |
|---|---|---|---|
| 100k | 134.5 us | 134.3 us | none measurable |
| 1M | 1.349 ms | 1.374 ms | 1.9% |

`simulation/snapshot_fresh` holds the previous snapshot while the next is built, exactly as the
channel does, so the allocator cannot hand back the block it has just freed. It is cheap anyway,
because it recycles a block of the same size every frame. A pool would buy under two per cent in
exchange for tracking when the renderer has finished with a snapshot.

That is the third recorded candidate in M8 to die on measurement, after hashing only the tables
a tick wrote and hashing in parallel. The pattern is worth naming: the things that looked
expensive because they are conspicuous — an allocation, a copy, a lock — were not, and the
things that turned out to matter were a per-byte multiply chain and a per-element capacity
check. Both were found by measuring rather than by reading the code and forming an opinion.

## The worker pool, M8: what parallelism costs and what it buys

`atlas::tasks` was created with this benchmark, which is the condition `docs/DEFERRED.md` set
for creating it at all. `atlas_bench --filter tasks`, Release.

**What a parallel loop costs before it does anything.** An empty `parallel_for`, so this is the
floor under every use of the pool:

| Chunks | Median |
|---|---|
| 1 | 0 ns — one chunk runs on the calling thread and never wakes anybody |
| 8 | 3.8 us |
| 64 | 13.8 us |
| 1024 | 146 us |

About 140 nanoseconds a chunk once threads are involved, plus a few microseconds to wake them.
A loop with less than roughly ten microseconds of work in it should not be parallel at all, and
the single-chunk case is free because it is not.

**What it buys**, over a million items, at two work densities. The first attempt measured only
the lighter one and would have reported the pool's ceiling as four workers; it is the machine's
memory bandwidth that stops there, not the pool.

| Workers | Memory-bound | | Compute-bound | |
|---|---|---|---|---|
| 0 (caller only) | 400 us | 1.0x | 6.61 ms | 1.0x |
| 1 | 200 us | 2.0x | 3.36 ms | 2.0x |
| 2 | 140 us | 2.9x | 2.30 ms | 2.9x |
| 4 | 98 us | 4.1x | 1.41 ms | 4.7x |
| 13 | 116 us | **3.4x** | 750 us | **8.8x** |

The compute-bound column is close to linear while the threads are performance cores — 4.7x from
five threads — and then falls off: 8.8x from fourteen, not 14x, because this machine's remaining
cores are slower ones and the chunks are equal. The memory-bound column stops improving at four
workers and gets worse at thirteen, because sixteen megabytes a call at 98 microseconds is
already about 160 GB/s and adding threads only adds contention.

**Which column the simulation sits in is the useful question, and it is the memory-bound one.**
The lab's systems walk cell columns and an adjacency structure: `accumulate_population` alone
reads sixteen megabytes of neighbour indices per tick. So M8 should expect something between two
and four times from parallelising the compute phase, not eight, and should measure rather than
assume — which is what this benchmark exists for.

**The thread sanitizer earned its keep here.** The first version of the pool passed all eight of
its functional tests and had a use-after-free: a worker leaves its claim loop by reading the
job's chunk count, and that read happens after the last chunk is counted, so the calling thread
could already have returned and destroyed the job on its stack. The fix is to wait for workers
to be *out* of the job rather than for the work to be *done*, and it costs the second row of the
dispatch table above.

## Parallel compute, M8: 2.07x, and what now stands in the way

The kernel takes an optional worker pool. A batch's systems run together, and a system may split
its own rows further through `ComputeContext::pool` — offered rather than applied, because a
system that draws from a random stream in sequence must not use it: the draw order is part of
the answer. The lab's two heavy systems split; its two random ones do not.

**The prediction, from the pool's own benchmark:** two to four times, not the eight that
compute-bound work reaches, because the lab's systems walk cell columns and an adjacency
structure and are therefore memory-bound.

| Workers | Tick at 1M cells | Speedup |
|---|---|---|
| none | 1.650 ms | 1.00x |
| 1 | 1.032 ms | 1.60x |
| 2 | 848 us | 1.95x |
| 4 | 694 us | 2.38x |
| 13 | 695 us | **2.38x** |

The bottom of the predicted range, and the reason is Amdahl's law rather than the pool: what is
left is serial. The hash is 0.38 ms of it, and the commit phase runs one system at a time by
contract, because commit order is what makes two systems writing the same table deterministic.

Those figures are after a second change that the first measurement provoked. Parallelising
compute left a 1.96 ms sequential tick at 2.07x, and the commit phase turned out to be copying
each system's scratch into its table — eight megabytes a tick between the two heavy systems, for
nothing, since compute rewrites every row before the next commit. Swapping instead of copying
took the sequential tick to 1.650 ms and the parallel one to 694 us, and moved the speedup from
2.07x to 2.38x because it removed serial work rather than parallel work. The contract that makes
the swap safe — every row rewritten every tick — is checked by predicting each row's new value
from its old one and requiring all of them to match; a deliberately skipped row fails it.

End to end, `atlas_lab --headless --grid 1024`, with both changes in: **588 ticks a second on
one thread, 1372 on five, 1387 on fourteen** — and the state hash is `0xd7f4f889cb0f848d` at all
three, which is M8's exit criterion demonstrated by the acceptance path rather than only by a
test. The first version of this paragraph quoted a binary that had not been rebuilt after the
commit change; the numbers here are from one that was, checked by watching it link.

### A conclusion this reverses

Earlier in M8, parallel hashing was measured and rejected: it would have taken the hash from
374 us to 113 us, 13% of a then 2.0 ms tick, in exchange for a worker pool that did not exist.
That was right at the time and is wrong now. The pool exists, and the tick it would be 13% of is
now 946 us, of which the hash is 40%. The ranking changed because the thing above it moved.

That is worth stating plainly because it is the ordinary condition of optimisation work, not a
mistake: a candidate's value depends on what else has been done, so a rejection is dated
evidence rather than a permanent answer. The two candidates now ahead of everything else are
hashing in parallel and not copying scratch during commit.

### On the test that nearly proved nothing

The worker-count invariance test first compared a pool of zero workers against pools of one, two
and four. All of them call `parallel_for`; only the absence of a pool skips it. So a deliberately
dropped row inside the split changed every run equally and the test passed. It now uses the
unsplit path as its baseline, and the same mutation fails two of its three cases. The test was
written, passed, and was believed before the mutation check was run.

## Regression thresholds, M8

`tools/bench_baseline.py compare` fails, with a non-zero exit, when a scenario is more than
**1.25x** slower than this machine's recorded baseline. That number is now measured rather than
assumed: four consecutive runs of all thirty-four scenarios spread by **1.03x at the median**
and never by more than 1.15x once the degenerate cases below are set aside. A quarter is
therefore comfortably clear of the noise while still catching anything worth looking at.

Two scenarios had spreads of 1.51x and 25.8x, and both measure **under two microseconds** — one
of them varied twenty-five fold while moving from nothing to nothing. A ratio taken on a handful
of timer ticks is a ratio of rounding, so anything under ten microseconds on either side is now
reported as "too small" and cannot fail a comparison. It is still printed, so a scenario that
grows from nothing into something remains visible.

**Thresholds are not enforced in continuous integration, and that is a decision rather than an
omission.** The exit criterion asks for thresholds "for CI where stable", and the evidence is
that nothing there is. The same hash comparison measures 31x on this machine and about 7x on a
shared Windows runner; the memory-bound scaling plateau lands in a different place on a
two-core runner than on this one. Gating on numbers that vary with whatever else the host is
doing would produce failures that say nothing about the change under review, which is how a
check becomes something people rerun until it passes. What continuous integration does run is
the hashing group's self-checks, which assert correctness and never timing.

So the workflow is: run `atlas_bench` on this machine **once it is idle**, compare against the
baseline, and treat a regression as a prompt to find out why.

The idleness is not fussiness. In M9 a comparison run immediately after a release build and two
container runs reported two regressions of 1.29x and 1.80x; three runs on a quiet machine put
the same scenarios within 1.02x of the baseline. The threshold did its job — it flagged
something real, and what was real was the load. A regression that does not reproduce on a quiet
machine is a measurement, not a change. Re-record the baseline only when the change that moved
it is understood and intended, which is why recording refuses a dirty tree.

## Optimisation candidates

Recorded as hypotheses, not commitments. Each requires a trace before it is attempted.

Five of the seven originally listed here have since been done, which is what the list is for:
batching draw submissions (M3), frustum and chunk culling (M7), compact data layouts (M8's
instance compaction), a faster state-hash algorithm (M8, `kHashAlgorithmVersion` 2), and
reduced synchronisation in the commit phase (M8's scratch swap, which removed the copy rather
than the synchronisation).

Still hypotheses:

- **Dirty-range snapshot publication.** Publish only the cells that changed. The lab's systems
  write every row every tick, so there is no dirty range to find in the one workload that
  exists; a workload with cold regions would change that.
- **Cached derived data.** Chunk aggregates are recomputed into the snapshot each publication.
  Never appeared in a profile.
- **Filling only the displayed palette band.** The snapshot carries all four bands so a map-mode
  switch rebuilds nothing. Filling one would cut snapshot work by roughly three quarters and
  make switching modes cost a rebuild. That is a decision about what the lab is for, not an
  optimisation, and it is not taken.
- **Hashing in parallel.** The largest remaining item at about 40% of the tick, and the only
  one gated on an owner's decision rather than a measurement: it changes the hash value and so
  spends `kHashAlgorithmVersion` 3. See `docs/DEFERRED.md`.
