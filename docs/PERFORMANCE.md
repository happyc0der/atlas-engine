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
policy, median, p90, p99, build configuration, dependency revisions, commit, and machine
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

The tail is much worse than the median, by a factor of ten at the smaller size. That is
expected on a machine with other things running and is the reason tails are reported at all,
but it is also the first thing to look at if these numbers ever need to improve.

The allocation counts are the evidence for M3's "no unbounded per-frame allocation" exit
criterion. Eight is not zero; what matters is that fifty times the quads does not mean fifty
times the allocations.

**What these numbers are not.** The first version of this benchmark timed a whole frame
including presentation and reported 8.3 milliseconds for every scene size, because that is
the display's refresh interval on this machine. A benchmark that measures the monitor is
worse than no benchmark, because it looks like data. Presentation is now outside the timed
section, and the scales differ as they should.

## Optimisation candidates

Recorded as hypotheses, not commitments. Each requires a trace before it is attempted:
batching draw submissions, frustum and chunk culling, compact data layouts, dirty-range
snapshot publication, cached derived data, a faster state-hash algorithm, and reduced
synchronisation in the commit phase.
