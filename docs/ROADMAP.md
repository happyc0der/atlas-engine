# Roadmap

Milestones are gated. Each ends with a clean configure, build, test run, sanitizer status
where applicable, and a runnable demonstration before the next begins. Sizes are relative
for a solo developer: S is days, M is one to two weeks, L is two to four weeks.

Status legend: **done**, *in progress*, planned.

| # | Milestone | Size | Status |
|---|---|---|---|
| M0 | Architecture and reproducible skeleton | M | **done** |
| M1 | Platform loop | M | **done** |
| M2 | Minimal GPU renderer | L | next |
| M3 | 2D camera and batching | M | planned |
| M4 | Asset pipeline | L | planned |
| M5 | Scene and serialization | M | planned |
| M6 | Simulation kernel | L | planned |
| M7 | Strategy Lab (engine v0.1) | L | planned |
| M8 | Performance hardening and parallel simulation | L | planned |
| M9 | Tooling and scripting decision | S–M | planned |

## M0 — Architecture and reproducible skeleton

Slices: tooling setup; documentation and ADRs; CMake skeleton and `atlas::core`; Catch2
and CTest; `Error`/`Result` and assertions; structured logging; sandbox lifecycle;
sanitizer presets; lint configuration and scripts; Tracy option; CI workflows; Claude hooks.

**Exit criteria — all met**
- Documentation and ADRs reviewed; ADR 0001, 0002, 0005 accepted.
- A fresh clone configures, builds, and tests through the documented presets.
- `atlas::core`, a sandbox application, a test target, formatting, static analysis, CI,
  sanitizer presets, and a compiled-out Tracy option all exist.
- Clean startup and shutdown with ordered, structured logs.
- `tools/precheck.sh` is clean; a deliberate boundary violation fails the check.

Verified on macOS arm64 (debug, release, ASan, TSan, profiling) and on Linux in a container
(Clang 19, debug). 41 unit tests pass in every configuration. Windows is covered by CI only.

Three portability constraints were found by building rather than by reasoning, and are
recorded in ADR-0001 and DEPENDENCIES.md: Clang 18 cannot compile `std::expected` against
libstdc++; plain Ubuntu 24.04 carries CMake 3.28, which sets the preset schema floor; and
CMake scans for C++20 modules by default under Ninja, which needs a tool Atlas does not
otherwise require.

## M1 — Platform loop

Slices: SDL3 dependency and the `platform` and `simulation` modules; time types and the
tick accumulator; platform lifetime and headless mode; window, events, input; resize,
minimize, focus; counters and documentation.

**Exit criteria — all met**
- SDL initialisation, window, events, input state, clocks, resize and minimise handling,
  and clean shutdown.
- A fixed-step accumulator with unit tests, independent of the renderer and of any clock.
- The sandbox runs headless, and window code is exercised in CI under SDL's dummy driver.

Verified on macOS with a real Cocoa window, which reported a logical size of 1280x720 and a
backing store of 2560x1440 at scale 2: the high-DPI distinction the API insists on is real
and correct on this hardware. A Linux container run covers the same code paths headlessly.

Two claims were weaker than they looked when the loop was first run, and were fixed rather
than reworded. The frame counters were documented as rolling and implemented as unbounded,
which grew to millions of samples in seconds. A headless realtime run spun the processor
flat out, producing 4.4 million empty frames to deliver 120 ticks; it now waits until the
next tick is due and uses no measurable processor time.

Minimise and restore are covered by injecting the events, which tests the translation. Only
a real window server produces them, so that part is exercised by hand rather than in CI.

## M2 — Minimal GPU renderer

Slices: `Handle`/`HandlePool` in core; the `rhi` module and device creation; frame and
clear; shader toolchain spike then the shader pipeline; triangle and buffers; profiling
zones and validation; lifetime, shutdown, and the shader ADR.

**Exit criteria**
- Clear and triangle through the Atlas RHI boundary.
- Debug validation enabled in debug builds.
- Resize and resource shutdown tested; the leak report is empty.
- CPU profiling zones around acquire, record, and submit. SDL_GPU exposes no timestamp
  queries, so GPU-side timing is a documented limitation rather than a claim.

## M3 — 2D camera and batching

Slices: shader toolchain hardening for resource bindings; `atlas::math`; orthographic
camera; texture and sampler upload; quad batching then instancing; the ImGui docking shell
with log and timing panels; an allocation check; the benchmark harness and first baseline.

**Exit criteria**
- Orthographic camera, texture, sampler, quad batching or instancing, and a debug overlay.
- No unbounded per-frame allocation after warm-up in the demonstrated path.
- A measured benchmark with machine-readable output and a recorded baseline.

## M4 — Asset pipeline

Slices: canonical hashing in core; an assets-internal I/O pool; virtual filesystem and
path normalization; asset IDs and the load state machine; texture and shader importers;
async load with main-thread GPU upload; fallbacks and failure paths; the dependency graph
and hot reload; the asset status panel.

**Exit criteria**
- Virtual paths, asset IDs, shader and texture import, async CPU loading, GPU upload,
  failure fallbacks, and development hot reload.
- Round-trip and failure-path tests.

## M5 — Scene and serialization

Slices: extract the `runtime` module; EnTT and the scene wrapper; hierarchy, cycle checks,
transform order; versioned canonical serialization; read-only editor panels; a sandbox
scene round trip.

**Exit criteria**
- A minimal scene ECS, transform hierarchy, stable IDs, versioned serialization, reload,
  and editor inspection.
- No simulation-domain assumptions in scene APIs.

## M6 — Simulation kernel

Scope is single-threaded. Slices: structure-of-arrays tables and read/write sets; the
schedule and its validation; commands; counter-based RNG streams; canonical state hashing;
replay; save and load; snapshot publication; the numeric policy ADR; the determinism suite.

**Exit criteria**
- Fixed integer ticks, commands, stable scheduling, seeded RNG streams, state hashing,
  replay, save/load, and immutable render snapshots.
- The same command log produces the same per-tick hashes across repeated runs, with the
  first divergent system reported on failure.
- Worker-count invariance is explicitly deferred to M8 and documented as a known limit.

## M7 — Strategy Lab — engine v0.1

Slices: a seeded synthetic grid; instanced cell rendering and map-mode switching; pan and
zoom; integer-ID picking with bounded readback; chunk culling hooks; mock systems over
data-oriented arrays; speed controls, replay, save/load, and hash display; a headless
benchmark CLI; the profiler counter UI.

**Exit criteria**
- Synthetic coloured cell field, map-mode changes, panning and zooming, ID picking, chunk
  culling hooks, mock data-oriented systems, speed controls, replay, and profiling UI.
- The headless simulation benchmark runs independently of rendering.

## M8 — Performance hardening and parallel simulation

Slices: define the first parallel benchmark; build the `tasks` module; execute the compute
phase in parallel; prove worker-count invariance; profile-guided optimisation only;
sanitizer and threshold hardening.

**Exit criteria**
- Profile-guided changes only, with before and after traces and benchmark data.
- Identical per-tick hashes across worker counts 1, 2, 4, and hardware concurrency minus one.
- ASan, UBSan, and TSan configurations clean for covered tests.
- Performance regression thresholds recorded for this machine and for CI where stable.

## M9 — Tooling and scripting decision

Slices: command and undo infrastructure with the first mutating editor action; editor
usability; the scripting ADR.

**Exit criteria**
- The editor workflow is usable for test scenes and synthetic datasets.
- An ADR decides whether Lua is justified, identifies its API boundary and security model,
  and either implements a tiny end-to-end script or explicitly defers it.

## Risks and deferred work

Top risks, with mitigations and the milestone where each bites, are tracked in the Gate 0
plan and summarised here:

- Shader toolchain on macOS has no prebuilt DXC; a from-source build or Slang is needed (M2).
- Cross-architecture float divergence between arm64 and x86_64 (M6); mitigated by
  integer-first authoritative state and `-ffp-contract=off` from M0.
- No GPU on hosted CI runners; mitigated by a software-Vulkan lane and local verification (M2+).
- No local Windows machine; MSVC-only breakage is found in CI. The owner decides by M3
  whether to acquire Windows hardware or a VM.
- Snapshot copy cost growth (M6–M8); mitigated by presentation-only snapshots and
  measurement before optimisation.

Deferred deliberately: render graph, custom allocator, custom ECS, work-stealing
scheduler, networking, physics, animation, plugin ABI, multi-viewport ImGui, filesystem
watchers, device-loss recovery, `.metallib` precompilation.
