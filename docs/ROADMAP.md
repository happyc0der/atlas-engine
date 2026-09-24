# Roadmap

Milestones are gated. Each ends with a clean configure, build, test run, sanitizer status
where applicable, and a runnable demonstration before the next begins. Sizes are relative
for a solo developer: S is days, M is one to two weeks, L is two to four weeks.

Status legend: **done**, *in progress*, planned.

| # | Milestone | Size | Status |
|---|---|---|---|
| M0 | Architecture and reproducible skeleton | M | **done** |
| M1 | Platform loop | M | **done** |
| M2 | Minimal GPU renderer | L | **done** |
| M3 | 2D camera and batching | M | **done** |
| M4 | Asset pipeline | L | **done** |
| M5 | Scene and serialization | M | **done** |
| M6 | Simulation kernel | L | **done** |
| M7 | Strategy Lab (engine v0.1) | L | **done** |
| M8 | Performance hardening and parallel simulation | L | **done** |
| M9 | Tooling and scripting decision | S–M | **done** |
| M10 | Charter amendment (ADR-0010) | S | **done** |
| M11 | Input: text, IME, gamepad | M | **done** |
| M12 | Audio | M | **done** |
| M13 | Animation | M–L | **done** |
| M14 | Networking: lockstep design and loopback proof | M | **done** |
| M15 | Sandboxed mods | L | **done** |
| M16 | Localisation: string tables, English | S | **done** |
| M17 | A transport for lockstep | M | **done** |
| M18 | Chess: the record, and M16's hotfix | S | **done** |
| M19 | A command may be declined | M | **done** |
| M20 | Chess: the rules library | M–L | **done** |
| M21 | A session can finish | M | **done** |
| M22 | Chess: the application, two people, a socket | L | **done** |
| M23 | One pin for vcpkg | S | **done** |
| M24 | A string API for mods | M | **done** |
| M25 | Dropping a peer and playing on | L | planned |
| — | Chess: a mod as the opponent | ? | deferred; numbered when scheduled |

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

**Exit criteria — all met**
- Clear and triangle through the Atlas RHI boundary.
- Debug validation enabled in debug builds.
- Resize and resource shutdown tested; the leak report is empty.
- CPU profiling zones around acquire, record, and submit. SDL_GPU exposes no timestamp
  queries, so GPU-side timing is a documented limitation rather than a claim.

The triangle is verified by reading the swapchain back and checking pixels, not by looking
at a screenshot: the apex must be red-dominant, the lower corners green and blue, and the
background the clear colour. Picking in M7 does not reuse this: it has its own deferred readback that never waits on a fence.

Direct3D 12 is not supported. The shader toolchain produces SPIR-V and Metal Shading
Language but not DXIL, because the compiler that produces DXIL has no macOS build; Windows
therefore uses the Vulkan backend. ADR-0006 records the reasoning and what would change it.

Resource binding conventions are untouched, because a triangle built from the vertex index
binds nothing. M3 is where that has to be settled.

## M3 — 2D camera and batching

Slices: shader toolchain hardening for resource bindings; `atlas::math`; orthographic
camera; texture and sampler upload; quad batching then instancing; the ImGui docking shell
with a timing panel; an allocation check; the benchmark harness and first baseline.

**Exit criteria — all met**
- Orthographic camera, texture, sampler, quad batching or instancing, and a debug overlay.
- No unbounded per-frame allocation after warm-up in the demonstrated path.
- A measured benchmark with machine-readable output and a recorded baseline.

Ten thousand textured quads reach the screen in one draw call, through an orthographic
camera with drag to pan and wheel to zoom about the pointer. The allocation claim is
measured rather than asserted: global operator new is replaced with a counting version and a
settled frame allocates eight times at a thousand quads and eight times at fifty thousand,
so the count does not grow with the scene.

The shader binding conventions M2 deliberately avoided are settled. Resource counts are read
out of the compiled shaders by reflection and reach the code as generated constants, so a
shader that gains a uniform cannot leave a stale count behind.

Three defects were found by running the code rather than by reading it. A shader reads a
uniform matrix column-major while Atlas stores row-major, so the first camera dropped its
translation and warped the field into a wedge; a rendered-pixel test now pins the layout
down and was confirmed to fail when the fix is reverted. Preparing the overlay's vertex data
uploads through a copy pass, which cannot nest inside the render pass that draws it, so the
overlay now hands back a token that makes the wrong order impossible to write. And the first
renderer benchmark measured the display's refresh interval rather than the engine: every
scene size reported the same 8.3 milliseconds.

## M4 — Asset pipeline

Slices: canonical hashing in core; an assets-internal I/O pool; virtual filesystem and
path normalization; asset IDs and the load state machine; texture and shader importers;
async load with main-thread GPU upload; fallbacks and failure paths; hot reload.

**Exit criteria — all met**
- Virtual paths, asset identifiers, texture import, asynchronous loading, GPU upload,
  failure fallbacks, and development hot reload.
- Round-trip and failure-path tests.

A texture now reaches the screen by being read and decoded on a worker thread and finalised
on the main thread, because only the main thread may create a graphics resource. Hot reload
was verified by replacing the file while the process ran and confirming the pixels on screen
changed.

Failure is not fatal. A missing or corrupt asset is recorded with a reason and draws a
deliberately hideous magenta fallback, so a frame still happens and the problem is obvious
rather than invisible. That was verified by pointing the sandbox at a directory with no
assets and checking the captured pixels.

Path handling is a security boundary and is treated as one: upward traversal, absolute
paths, drive letters, backslashes and null bytes are all refused, and a resolved path is
checked to lie inside its mounted root rather than merely to have been built from one,
because a symbolic link can point anywhere. Each of those has a test.

The asset workers run under ThreadSanitizer as part of the sanitizer job.

Deferred with reasons rather than silently: there is no asset status panel, because the
overlay's only consumer so far is the sandbox, which reports asset counts in its exit
summary, and a panel would be a second place for the same three numbers to be wrong; shader
loading still goes through the generated
manifest from M3 rather than the registry, because the registry has nothing to add to a
shader whose resource counts are already compile-time constants; cooked-artifact caching is
not implemented, because nothing yet takes long enough to import to justify a cache; and the
dependency graph is not built, because with one asset type nothing depends on anything.

## M5 — Scene and serialization

Slices: EnTT and the scene wrapper; hierarchy, cycle checks, transform order; versioned
canonical serialization; a read-only scene panel in the overlay; a sandbox scene round trip.

**Exit criteria — all met**
- A minimal scene ECS, transform hierarchy, stable IDs, versioned serialization, reload,
  and editor inspection.
- No simulation-domain assumptions in scene APIs.

Entities are referred to by a `StableId` the scene assigns, never by the entity library's
handle. The handle is recycled as entities come and go, so a file that stored one would load
without complaint and refer to the wrong things. Everything observable is ordered by that
identifier: iteration, drawing, sibling lists, and the saved file. See
[ADR-0004](adr/0004-scene-ecs-vs-simulation-storage.md).

The file format is JSON that names and versions itself, refuses a version it does not know,
and is canonical: the same scene produces the same bytes whatever order it was built in.
That property is tested by building one scene two ways and comparing the output, not only by
saving twice, because saving twice passes under any fixed order. See
[ADR-0007](adr/0007-scene-file-format.md).

A scene file is untrusted input. Malformed JSON, a wrong marker, a missing version, a
reserved or duplicate identifier, a parent that does not exist, a cycle, an over-long name
and an out-of-range layer are each refused with a reason, and a failed load leaves the target
scene exactly as it was rather than half-populated.

The sandbox proves the whole path in one run under `--scene`: it builds a hierarchy in code,
saves it, reads the file back, saves the loaded copy again, refuses to start if the two do
not match byte for byte, and then draws the loaded copy. The overlay's scene panel shows the
tree and the components of a selected entity, including its composed world transform.

**Fixed in passing: frame capture was doing something Metal forbids.** Reading a frame back
copied from the swapchain image, which Metal creates framebuffer-only. Without the validation
layer the copy appeared to work; with it enabled, four GPU tests aborted. Capture now draws
into an offscreen colour texture and blits that to the swapchain, which is legal, keeps the
frame visible, and is the same offscreen target M7's picking needs. The defect predated M5 and
was found by turning the GPU test preset's Metal validation on.

**Known gap: the batcher cannot draw a rotated sprite.** A scene may hold any rotation and
composes it correctly, but `renderer::Quad` is an axis-aligned rectangle, so drawing takes
position and scale from the composed matrix and drops rotation. The sandbox scene therefore
uses no rotation rather than displaying something that does not match what it holds. Rotated
instances are picked up when the renderer next changes, in M7.

Deferred with reasons rather than silently:

- **The `runtime` module was not extracted.** The plan put it here on the assumption that a
  second application would need the same composition. Scene inspection went into the existing
  overlay instead, so there is still exactly one composition root. Extracting it now would
  produce an abstraction with one call site, which this project's own rules forbid. It is
  created when a second application genuinely needs it.
- ~~**No command or undo infrastructure, and so no editing.**~~ **Resolved in M9.** The panel
  now takes an `edit::History`, which exposes its scene as const and has no method yielding a
  mutable one, so the compiler still enforces that a widget cannot bypass validation — the
  guarantee is kept rather than traded away. Adding widgets first would have created a second
  way into the scene that bypasses the checks `Scene` performs.

## Closing two gaps found by auditing M0 to M4

Audited against the plan's slice lists rather than the milestone reports, which found work
that had been listed as done and was not.

**Integration tests now exist.** M0 and M1 both called for them and neither delivered any.
Nothing ran the sandbox binary under the test runner, so subsystem construction order,
shutdown order, exit codes and lifecycle logging were verified only by hand. Eleven checks
under the `integration` label now run the binary end to end: build identity, help, the
ordered lifecycle, exact tick counts, unbounded throughput, refusal of an unbounded headless
run, rejection of unknown options and out-of-range values, the log file, severity filtering,
and a real window under the dummy video driver. Continuous integration used to do a little of
this by hand in a separate step; that step is gone, because the tests cover it and a developer
now gets the same coverage locally.

The first thing they caught was a real defect: `--ticks N` meant exactly N when pacing against
the clock and N rounded up to the batch size under `--unbounded`, so asking for twenty
thousand ticks ran twenty thousand and thirty-two. A benchmark dividing by N would have been
quietly wrong. The count is now clamped and the flag means one thing.

**Device loss is detected.** `ErrorCode::DeviceLost` had existed since M2 and nothing could
ever return it, which is a worse state than not having it: an error code nothing produces
looks like handled behaviour. The first failure that says the device is gone now latches, and
every later call fails immediately with the original reason instead of attempting work and
failing differently. Recovery stays out of scope, as the charter says.

What can be detected is bounded by what the graphics library reports, and the limits are
recorded rather than papered over: reliable on Vulkan, best effort on Direct3D 12, and
unavailable on Metal, which is the backend this project develops on. The classifier is a pure
function over the library's message and is tested in both directions, because a false positive
latches and takes the rest of the run down with it.

All three items that audit left open were closed on 2026-09-15.

**The renderer is now verified on a second backend.** It had been developed entirely against
Metal on one machine. It now runs on Vulkan against Mesa's llvmpipe, a complete software
implementation, inside a container with no graphics hardware and no display: all 34
GPU-labelled tests pass, and the sandbox draws a frame and reads it back. One script does this
locally and in continuous integration, so the two cannot drift. What it does not catch is
driver behaviour, performance, or anything about Metal; llvmpipe is a correct implementation,
not a representative one.

**The Stop hook** refuses to finish on a tree that does not build and pass, which the
completion checklist has always required and nothing enforced. It skips when nothing affecting
the build has changed, so a documentation edit does not pay for a compile, and it never blocks
twice in a row.

**The Windows formatting script** matches the shell script's behaviour, directories,
extensions and version pin, verified against stub formatters in a container since this machine
has no PowerShell.

## M6 — Simulation kernel

Scope is single-threaded. Slices: structure-of-arrays tables and read/write sets; the
schedule and its validation; commands; counter-based RNG streams; canonical state hashing;
replay; save and load; snapshot publication; the numeric policy ADR; the determinism suite.

**Exit criteria — all met**
- Fixed integer ticks, commands, stable scheduling, seeded RNG streams, state hashing,
  replay, save/load, and immutable render snapshots.
- The same command log produces the same per-tick hashes across repeated runs, with the
  first divergent system reported on failure.
- Worker-count invariance is explicitly deferred to M8 and documented as a known limit.

A tick is four steps in a fixed order: drain and apply the commands stamped for it, compute,
commit, hash. Compute receives a `const World` and commit a mutable one, so a system cannot
write shared state while another reads it. The compiler enforces that rather than a rule
doing it, and it is the single reason moving compute onto workers in M8 is a scheduling
change instead of a redesign.

Random numbers are counter-based, keyed by seed, stream, tick and counter. With one shared
stateful generator, adding a call anywhere shifts every later value everywhere and a replay
stops matching for a reason unrelated to the change. A test pins that property directly.

Batches are derived and validated even though M6 runs everything sequentially, so M8 has
nothing left to design and a wrong access declaration is caught now rather than becoming a
data race later.

The determinism suite tests the mechanism as well as the claim. It corrupts a recorded
checkpoint and requires the playback to report the tick and name the first system whose
writes differ; it alters a command and alters the seed and requires each to diverge, because
a playback that ignored its own log would pass every other test.

**Measured: the fixed golden scenario produces identical hashes on macOS arm64, Linux arm64
and Linux x86_64**, across two compilers and two standard libraries. The arm64 and x86_64
agreement is the pair the numeric policy was written for, since one contracts multiply-add by
default and the other does not. Windows and MSVC remain unmeasured, and the scenario is
integer-only on purpose, so this says nothing yet about floating point in authoritative state.
The numbers and those limits are in [DETERMINISM.md](DETERMINISM.md).

Deferred with reasons rather than silently:

- **The snapshot channel uses a mutex, not an atomic shared pointer.**
  `std::atomic<std::shared_ptr<T>>` is the natural fit and is unavailable: the development
  platform's standard library does not define `__cpp_lib_atomic_shared_ptr`, checked rather
  than assumed, and the deprecated free-function overloads are removed in C++26. The lock is
  held for the length of a pointer copy, once per tick and once per frame.
- **No save migration code.** There is no second format version to migrate from, and writing
  migration for an imagined change would be writing untested code. The version check is what
  makes deferring it safe: a file this build cannot read is refused rather than misread.
- **Worker-count invariance belonged to M8**, and M8 proved it. M6 proves single-threaded replay
  determinism and ships the contract that M8 needs; doing both at once would double the
  debugging surface.

## M7 — Strategy Lab — engine v0.1

Slices: a seeded synthetic grid; instanced cell rendering and map-mode switching; pan and
zoom; integer-ID picking with bounded readback; chunk culling hooks; mock systems over
data-oriented arrays; speed controls, replay, save/load, and hash display; a headless
benchmark CLI; the profiler counter UI.

**Exit criteria**
- Synthetic coloured cell field, map-mode changes, panning and zooming, ID picking, chunk
  culling hooks, mock data-oriented systems, speed controls, replay, and profiling UI.
- The headless simulation benchmark runs independently of rendering.

### Gate 1 — engine capability, done before any lab code

Planning M7 began with a re-examination of M0 to M6 that found fourteen things, the largest
being that nothing outside the tests had ever driven the M6 kernel, and that the quad batcher
had stalled on a graphics fence once per flush since M3 with the evidence already in the
committed benchmark baseline: ten times the quads moved the p99 by minus eight percent, a tail
flat in the payload, which is a fixed cost. The owner chose two gates so that the engine work
could be proven and reported before the lab depended on it.

What Gate 1 built, each with tests:

- **Offscreen colour targets, `R32Uint`, and a blend mode** (`TextureUsage`,
  `ColourTargetDesc::texture`, `BlendMode`, `RenderPass::target_extent/target_format`).
  Spiked on Metal and llvmpipe first, because an integer target through the shader
  cross-compiler was the one unknown. Two backend disagreements were measured and shaped the
  API; ADR-0002 records both.
- **Deferred bounded readback**, polled through `SDL_QueryGPUFence`, at most four outstanding,
  the pixel storage sized at request so the poll is genuinely allocation-free. A 1x1 readback
  returns exactly four bytes, which is what picking needs. Capture was rebuilt on the same path,
  which fixed its assumption that every texture has the swapchain's format.
- **Streaming buffer upload** through SDL's cycling, with the prediction written down first:
  a fixed wait should collapse the p99 toward the p90. The committed baseline did not reproduce
  (a ninety percent spread across runs), so per the plan the comparison stopped there and the
  fence was measured directly at about 460 microseconds per flush, which is the cost removed.
  The test that matters is two flushes in one frame with both groups asserted present in the
  picture; the capacity test alone would have passed while the first draw showed the second
  flush's data.
- **`TickAccumulator::set_tick`**, so a load cannot leave the two tick counters diverged.
- **The cooked-artifact cache.** Built as the charter requires, measured, and found wanting in
  its first form: hashing the source bytes for the key cost 15.3 milliseconds against an
  8.25 millisecond decode. Reported, and re-keyed on the owner's decision to size plus
  modification time plus importer version, after which a warm import at 2048² is 2.26
  milliseconds against 8.46 cold. Both numbers and the rejected design are in
  `docs/PERFORMANCE.md`. A cache entry is untrusted input: dimensions, byte count and file
  size are all checked before a byte is trusted, and a corrupt entry is discarded, tested.
- **`apps/common`** instead of the `runtime` module: the roughly 195 lines the two applications
  would have duplicated, none of which had a test, now a static library with thirteen test
  cases. The sandbox lost 196 lines and its eleven integration cases still pass unchanged.
- **A documentation truth pass**: the ten-versus-twelve CI count, three places that promised
  capture carries picking, a backwards `tools → runtime` edge in the module graph, the read-set
  wording in `schedule.hpp`, the shader loader's "stopgap" note, and a claim in
  `docs/PERFORMANCE.md` about a field that does not exist.

Two designs were changed by measurement rather than argument: an integer clear value was
removed from the descriptor because the backends do not agree on it, and the cache key was
changed because the content hash cost more than the work it saved. Gate 2, the lab itself,
started only after this gate was reported.

### Gate 2 — the Strategy Lab

`apps/lab` is three targets. `atlas::lab_sim` (tables, systems, command, snapshot) may link
only `atlas::simulation`, and CMake refuses anything more, so the headless benchmark is
independent of rendering by construction. `atlas::lab_view` (the cell field, the identifier
pass) exists so the GPU tests draw with the application's own code. `main.cpp` composes them.

What it proves, with no game rules anywhere:

- **A seeded synthetic grid**, chunk-major, so a visible chunk is one contiguous range, a
  cell's identifier is derivable from an instance index plus a per-chunk base, and M8 gets
  disjoint ranges for free. Five tables with neutral names; adjacency is a compressed-sparse-row
  structure whose five invariants are each checked with their own message and corruption test.
- **A million cells drawn**, culled per chunk, through the streaming path
  Gate 1 built. Pan, zoom, and four map modes that switch by reading a different band of the
  same snapshot: geometry is built once and a mode switch rebuilds nothing, tested directly.
- **Picking through an integer-identifier target** with a deferred readback that arrives two
  frames later and is never waited on. Every pick is cross-checked against the analytic inverse
  of the projection; the GPU tests check 676 sampled pixels plus cell edges and chunk
  boundaries; the integration check reimplements the index formula in Python.
- **Four mock systems**, one per access pattern, all integer arithmetic. The schedule's
  batching was observed and then asserted: three batches, the two writers of `cells`
  serialised because write sets are table-granular. Recorded for M8, which did not take it up:
parallelism within a batch proved the larger prize. See docs/DEFERRED.md.
- **The kernel's tick is authoritative.** The accumulator only decides how many ticks to run,
  is committed with what the kernel ran through the shared clamp, is re-synchronised after a
  load, and is compared against the kernel every frame under a debug assertion. A save at 200
  ticks, loaded and run 100 more, matches an uninterrupted 300, with commands on the way.
- **Time controls, replay, save and load, hashes.** Pause, single step, four speeds,
  unbounded; a replay that plays back clean, refuses the wrong starting state up front, and on
  a semantic tamper (one command's colour byte, kept in range) names the tick it diverged at.
  A failed load changes nothing, at the application level too: the state is serialised before
  a load and restored if the cross-table check refuses what `sim::load` accepted.
- **Snapshot publication** once per frame after the last tick, never headless: 1.43 ms at a
  million cells, in the overlay as a counter.
- **Profiler counters**: a stats panel with the state hash, speed, mode, visible chunks, batch
  figures and a processor-side phase breakdown with the largest phase marked, plus profiling
  zones around every phase.
- **A headless benchmark independent of rendering**: `atlas_lab --headless` is the acceptance
  path and prints an observation clearly labelled as one; `atlas_bench --filter simulation` is
  the measurement path. Its first finding is that the world hash is 88% of a million-cell tick.

Tests: 36 cases in the simulation library including a pinned golden scenario that a
one-constant mutation fails; 4 GPU cases; 22 integration cases, of which `seed_changes_hash`,
`load_continues`, `replay_detects_tampering` and `pick_returns_expected_cell` are the ones
designed to fail without a specific piece of the design. The golden hashes match on macOS
arm64, Linux x86_64 and Windows x64 through continuous integration.

What the lab found about the engine, which is what it is for: the world hash dominates the
tick (M8's first question); table-granular write sets serialise systems that touch different
columns (M8 candidate); a million 48-byte instances a frame is 48 MB the identifier pass shows
how to avoid (M8 candidate); and the sandbox's zoom anchors in logical units against a pixel
viewport (recorded). All in `docs/DEFERRED.md`.

### Engine v0.1, against the charter

| Charter requirement | Status |
|---|---|
| Reproducible build and test workflow on all three tier-one platforms | **Met.** Four workflows, six jobs, green on every push; presets, pinned vcpkg baseline, containers for Linux and llvmpipe from the development machine. |
| Window, input, events, clocks, clean startup and shutdown | **Met.** Two composition roots, both with lifecycle integration tests. |
| Engine-owned renderer boundary over SDL_GPU: buffers, textures, samplers, shaders, pipelines, render passes, uploads, and integer-ID readback | **Met.** Offscreen targets, `R32Uint`, blend modes, streaming uploads and deferred readback arrived in Gate 1; the lab picks through them. |
| Asset system: virtual paths, stable IDs, async CPU loading, cooked-artifact caching, fallbacks, hot reload, for shaders and textures | **Met for textures; shaders are loaded by path from the cooked manifest, not through the registry.** The deferral and its reason are recorded; the loader became public renderer API in M7 when the lab became its second caller. |
| Scene layer with transform hierarchy and versioned serialization using stable IDs | **Met** (M5). |
| Simulation kernel: fixed integer ticks, commands, deterministic system ordering, seeded RNG streams, canonical state hashing, replay, save/load, snapshot publication | **Met** (M6), and now driven by a real program. |
| Strategy laboratory: synthetic cell field, map modes, pan and zoom, ID picking, mock data-oriented systems, speed controls including unbounded headless execution, replay, profiler counters | **Met**, as above. Controls were keyboard only at v0.1 and deliberately so; M9 added the widgets once the command infrastructure existed to make them safe. |
| Documentation and ADRs that match the implementation | **Met** as of this commit; the E7 truth pass and this section are the evidence, and the next divergence will be found the same way. |

One requirement is met with a stated qualification (shaders bypass the registry) and none is
unmet. Not claimed, because not measured: bit-identical simulation across compilers beyond the
golden scenario on three platforms; graphics-processor time; and anything about
multi-threaded execution, which M8 went on to measure.

## M8 — Performance hardening and parallel simulation

Slices: define the first parallel benchmark; build the `tasks` module; execute the compute
phase in parallel; prove worker-count invariance; profile-guided optimisation only;
sanitizer and threshold hardening.

**Exit criteria**
- Profile-guided changes only, with before and after traces and benchmark data.
- Identical per-tick hashes across worker counts 1, 2, 4, and hardware concurrency minus one.
- ASan, UBSan, and TSan configurations clean for covered tests.
- Performance regression thresholds recorded for this machine and for CI where stable.

### What was built

M7 handed over a million-cell tick of 13.05 ms and a headline finding: the world hash was 88%
of it. Every change below was made because a measurement pointed at it, and three candidates
were killed by measurement instead. Full numbers and method are in `docs/PERFORMANCE.md`.

- **A new hash algorithm, `kHashAlgorithmVersion` 2.** Four independent chains over 32-byte
  blocks with a final mix, replacing byte-at-a-time FNV-1a for bulk data. **11.6 ms to
  0.37 ms.** `hash_string` stays FNV-1a: identifiers are short, compile-time, and their cost
  never appeared in a profile. Avalanche improved from 30.67 to 32.02 bits, measured, because
  a faster hash that mixes worse is not a faster hash. The golden values were re-recorded, and
  the change was proved value-only by restoring the old algorithm and watching the old values
  come back.
- **`atlas::tasks`**, a fixed worker pool with a deterministic `parallel_for`. The partition
  depends only on the count and the grain, never on how many workers there are, which is what
  makes worker-count invariance a property of the work rather than of the schedule. Dispatch
  costs about 140 ns a chunk, measured before anything was built on it. A use-after-free found
  by ThreadSanitizer fixed the waiting rule: the caller waits for workers to be out, not merely
  for chunks to be done.
- **The compute phase runs on the pool.** A batch holding more than one system is dispatched
  across workers, and each lab system splits its rows at a grain of 16,384. Commit stays serial
  in declared order, because that order is what makes two systems writing one table
  deterministic. **2.07x**, the bottom of the predicted range.
- **Commit swaps instead of copying.** The 2.07x measurement showed the remaining serial work,
  and most of it was the commit phase copying each heavy system's scratch into its table: eight
  megabytes a tick, for nothing, since compute rewrites every row first. Swapping took the
  sequential tick from 1.960 ms to **1.650 ms** and the parallel one to **694 us**, moving the
  speedup to **2.38x** — because it removed serial work, which is the only kind that changes a
  ratio. The contract that makes the swap safe is enforced by a test that predicts every row's
  new value from its old one.
- **Instance compaction for the cell field.** Four bytes a cell instead of a 48-byte quad, with
  the rectangle derived in the shader from the instance index. Submission **7.11 ms to 303 us**
  and the per-frame buffer 48 MB to 4 MB. Two thirds of the gain came from what compaction
  revealed rather than from compaction itself: a `push_back` per cell.
- **Regression thresholds**, and this is where a guess became a measurement. The 1.25x gate is
  now justified by four consecutive runs of all thirty-four scenarios: 1.03x spread at the
  median, never above 1.15x. The two scenarios that exceeded the gate on noise alone both
  measure under two microseconds, so anything under ten microseconds is now reported but cannot
  fail a comparison.

**Measured and deliberately not built:** hashing only the tables a tick wrote (saves nothing —
the lab writes almost every table every tick); snapshot pooling (1.9%, because the allocator
already recycles); parallel hashing (rejected at 2.0 ms, then reopened when the tick shrank —
see below).

**A correction M8 made to M7's report.** M7 recorded that drawing a million cells cost 8.3 ms.
It did not: 8.3 ms is 120 Hz, and the frame was waiting for the display. Four thousand cells
produced the same figure, which is what gave it away. Three documents were corrected and the
cell-field benchmark now excludes presentation.

### Exit criteria, against what was done

| Criterion | Status |
|---|---|
| Profile-guided changes only, with before and after | **Met.** Every change above has a recorded before, after, and a prediction written down in advance; three candidates were dropped when the measurement disagreed, and one rejection was reversed when the tick it was weighed against no longer existed. |
| Identical per-tick hashes across worker counts 1, 2, 4, and hardware concurrency minus one | **Met**, in tests and by the acceptance path: `atlas_lab --headless --grid 1024` reports `0xd7f4f889cb0f848d` at 0, 4 and 13 workers, at 588, 1372 and 1387 ticks a second. |
| ASan, UBSan and TSan clean | **Met.** The address preset has always been `-fsanitize=address,undefined`, so every "ASan clean" result this milestone was also UBSan clean; the thread lane found the pool's use-after-free before any parallel result was reported. |
| Regression thresholds for this machine and for CI where stable | **Met for this machine**, with the threshold derived from measured noise. **Not enforced in CI, as a decision:** the same comparison measures 31x here and about 7x on a shared Windows runner, and a check that fails for reasons unrelated to the change is one people rerun until it passes. |

**Cumulative: the million-cell tick went from 13.05 ms to 694 us, about 19x**, and the cell
field's submission path from 7.11 ms to 303 us separately.

**Left open, deliberately.** Parallel hashing is now the top candidate at roughly 40% of the
tick, and it is not taken: every scheme that splits the hash changes its value, which spends
`kHashAlgorithmVersion` 3 and invalidates saves and replays. That is an owner's decision, not a
performance one. The render graph and column-level read/write sets were both marked "live for
M8" and both went untaken; `docs/DEFERRED.md` records why and what would change it.

## M9 — Tooling and scripting decision

Full report: [reports/M9.md](reports/M9.md).

Slices: command and undo infrastructure with the first mutating editor action; editor
usability; the scripting ADR.

**Exit criteria**
- The editor workflow is usable for test scenes and synthetic datasets.
- An ADR decides whether Lua is justified, identifies its API boundary and security model,
  and either implements a tiny end-to-end script or explicitly defers it.

### A re-examination first

M9 opened the way M7 did, with a sweep for promises whose milestone had passed. It found
twenty-two things, six of which changed behaviour, and they were fixed before any editor code
was written on top of them.

- **The sandbox panned and zoomed at half speed on a high-density display**, dividing a logical
  pointer delta by zoom against a viewport measured in pixels. The recorded deferral covered
  only the zoom anchor and said "the lab converts; the sandbox should too" — it understated the
  scope. Both of the sandbox's scene classes had it, pan and zoom each, so it was four sites.
- **Seven entry points called into the platform or the graphics device with no main-thread
  assertion**, while their immediate neighbours had one. Two documents claimed "every entry
  point asserts main-thread affinity". That sentence is now true without qualification.
- **The device-loss API had no caller anywhere.** `is_lost()` and `loss_reason()` were
  referenced only by their own definitions while `ARCHITECTURE.md` presented "Atlas detects it
  and stops" as a live mechanism. Both applications now ask, on the failed frame where loss
  surfaces, and exit with their own code.
- **Worker-count invariance skipped "hardware concurrency minus one"** on any machine with four
  or fewer cores — exactly the continuous-integration runners.
- The sandbox never called `wait_idle` at shutdown though the lab does and the architecture
  flowchart says to; and the headless pacing block had been copied into both applications with
  the bound drifted to 5 ms in one and 4 ms in the other.

The worst of the rest was not a defect but a claim: **`docs/DETERMINISM.md` published the
version 1 state hashes under the project's cross-architecture agreement claim**, while the
golden test had asserted different values since M8 replaced the algorithm. Probably still true,
with stale evidence, which is the most misleading combination available.

The module diagram had drifted five ways against the table it claimed to depict. It is now
regenerated from `cmake/ModuleGraph.cmake`, and `tools/check_module_deps.py` compares them, so
it cannot drift again without `precheck` failing. Its first catch was `atlas::edit`, added
later in this same milestone and missing from the diagram.

### What was built

**`atlas::edit`**, a new module between `scene` and `tools`. Not inside `scene`, which is the
data model and should not know it is being edited; not inside `tools`, which links Dear ImGui
and the graphics headers and therefore had no tests at all. A command captures what it needs to
reverse itself the first time it is applied and then owns both directions — not whole-scene
snapshots, which would copy every entity to destroy one leaf, and not separate inverse objects,
since the inverse of a destroy needs data only the pre-destroy scene has.

Three things the scene's own API dictated. Most `Scene` setters return `void` and silently do
nothing for an entity that is not there, so every command checks existence itself; without that,
an edit built from a selection the user had already deleted would report success and enter the
history with nothing to undo. `destroy` cascades, so its undo restores a subtree with the
original identifiers, which is safe because identifiers are never reused and `set_parent` sorts
siblings by identifier. `set_parent` refuses a cycle before detaching anything, so a refused
reparent never reaches the history.

**Scene editing from the overlay.** The panel's signature went from `const scene::Scene&` to
`edit::History&`, and the guarantee the const reference existed for is kept rather than waived:
the history exposes its scene as const and has no method yielding a mutable one, so a widget
still cannot reach past validation and the compiler still enforces it. The first widget is the
local position, with undo and redo buttons — the first interactive controls the overlay has
ever had beyond a tree node. Coalescing was built in from the start, because the first widget
is a drag and without it the first thing a user learns about undo is that one drag takes forty
presses.

**Three panels.** A log console, which needed no new plumbing: `core` has had a bounded,
thread-safe `LogBuffer` and a weak-referencing sink since M0, with a comment naming the console
that would one day read it. An asset status panel, which the registry was built for. And
simulation controls for the lab, which was keyboard-only. The controls panel returns a request
and changes nothing itself — `tools` cannot depend on an application, so it could not call the
lab's functions if it wanted to — and the lab's key handling now builds the same request type,
with one function applying both, so a button and a key cannot come to mean different things.

**[ADR-0009](adr/0009-scripting-decision.md)** defers embedded scripting and fixes the boundary
now: nothing outside the tick reaches simulation state except through a command, and nothing
runs during a tick.

### Exit criteria, against what was done

| Criterion | Status |
|---|---|
| The editor workflow is usable for test scenes and synthetic datasets | **Met.** The sandbox's scene is editable through an undoable history, and the lab's time controls, display modes, save and load are buttons rather than undocumented keys. A log console and an asset browser exist in both. What "usable" does not yet include is renaming, which needs a platform text-input event that does not exist; that is recorded rather than glossed. |
| An ADR decides whether Lua is justified, identifies its API boundary and security model, and either implements a tiny script or explicitly defers it | **Met**, by the explicit-deferral branch the criterion allows. The boundary and the security model are both recorded in full, so the decision can be evaluated later against something concrete rather than re-argued. |

**Not claimed.** The drag widget itself was verified by hand and by captured frames, not by an
automated test: driving a Dear ImGui control programmatically needs precise synthetic pointer
input and would test the mock more than the panel. What *is* tested automatically is everything
either side of it — the commands and the history headlessly, the filter logic headlessly, and
the panel's render path under the `gpu` label on two backends.

### What this milestone deliberately did not build

Rename, rotation, scale, sprite and camera widgets, reparent by drag, multi-selection, a
clipboard, memory counters, a renderer resource view, record and play as buttons, a reset
button, and dock persistence.
Each has its reason in [DEFERRED.md](DEFERRED.md). The commands behind several of them exist
and are tested, which is recorded there so that "a command with no widget" reads as intended
rather than as a gap.

## After M9

**Every milestone the original roadmap planned is done.** M0 through M9, ten of them, each
ending green on four continuous-integration workflows across macOS arm64, Linux x86_64 and
Windows x64. Engine v0.1 was declared at M7 against the charter item by item; M8 made the
simulation parallel and nineteen times faster at a million cells; M9 made the engine editable
and decided the scripting question.

**A second series, M10 to M16, was added on 2026-09-17 by owner decision**, recorded in
[ADR-0010](adr/0010-charter-amendment.md). The section below was written when M9 closed and the
table ended there; it is kept because its reading of where the project stood is what the new
series was decided against, and because the question it raises has not been answered, only
postponed.

What that does not mean is that the engine is finished, and this section exists so that nobody
reads a table of ticks and concludes otherwise.

**What is genuinely open, and why it stays open.** Everything consciously not built is in
[DEFERRED.md](DEFERRED.md) with the condition that would change the decision. Three items are
worth naming here because they are the largest, and because two of them are not engineering
questions at all:

- **Hashing in parallel** is the biggest measured performance win left, at roughly 40% of a
  694-microsecond tick. It is not taken because every scheme that splits the hash changes its
  value, spending `kHashAlgorithmVersion` 3 and invalidating every save and replay. That is a
  decision about what breaking a stored format is worth, which belongs to whoever owns the
  project rather than to whoever is optimising it.
- **Embedded scripting** was decided for now by [ADR-0009](adr/0009-scripting-decision.md),
  with a recorded trigger — deliberately a limitation somebody hits, not a date. **That trigger
  was fired by decision rather than by a limitation on 2026-09-17**; see ADR-0010's honest
  accounting of which of its three conditions were actually met.
- **Direct3D 12 and non-Apple graphics hardware.** The renderer has been verified on one
  graphics processor and one software rasteriser. This is the largest untested surface in the
  project and no amount of continuous integration on the current runners changes it.

**What the next milestone would be, if there is one.** The charter's purpose is an engine for a
map-based grand-strategy game, and the engine now has every capability that charter names. The
next real question is not a milestone in this list: it is whether to start the game, and the
answer to that shapes what the engine needs next. A game would immediately exercise the things
the Strategy Lab only stands in for — many more tables, systems with genuinely different
access patterns, a save format that has to migrate — and each of those has a deferral waiting
for exactly that evidence.

**What was decided instead.** The owner chose to extend the engine first, with seven
subsystems. The paragraph above is not retracted: it is still the case that a game is the only
thing that can show whether these abstractions are right, and ADR-0010 records that risk rather
than arguing it away. Completing M10 to M16 is therefore **not** v1.0; v1.0 is declared when a
game links the engine and runs without patching it.

## M10 — Charter amendment

Slices: [ADR-0010](adr/0010-charter-amendment.md), and the five documents it changes.

**Exit criteria**
- The charter, README, DEFERRED, ADR index and this table say the same thing about what the
  engine plans to be, and each change is one the ADR tabled in advance.
- ADR-0009 carries a dated forward pointer rather than an edit that hides the change of mind.
- `tools/precheck.sh` clean. No code changes; no test result changes.
- Stop and report, so the amended charter is read before a line of feature code exists.

### Exit criteria, against what was done

**All met**, at `2a2b1d0`. Five documents changed exactly as the ADR tabled them in advance;
ADR-0009 carries a dated forward pointer under its status rather than a rewritten decision, and
the index legend gained "Superseded in part by NNNN" so that a partial change of mind has
somewhere to be recorded; `precheck` clean at 554 tests, none of which moved because no code
did.

The record says plainly what it cost. The charter has now been amended **by decision rather
than by evidence**, and the next amendment will cite this one as precedent. That is the real
price of the second series and it was written into the record rather than argued away.

## M11 — Input: text, IME, gamepad

Full report: [reports/M11.md](reports/M11.md).

Slices: text input and the rename widget; input-method candidate positioning; the gamepad in
the platform; the consumers, which is one camera controller and one binding table.

**Exit criteria**
- `Event` carries text and gamepad alternatives, with its name table, its positional
  assertions and its coverage test moving together, and with its trivial copyability asserted
  rather than narrated.
- Typing into a real panel works end to end, proven by a GPU test rather than by inspection.
- Text input is on only while something is focused, in both applications.
- A controller pans and zooms everywhere the mouse does, with no anchor drift.
- What cannot be automated is written down as such, not glossed.

### The gap this closes

M9 shipped an editor that could not rename anything, and said so in its own report. The reason
was three layers down: the platform had no text event, so it forwarded no characters; the
overlay's key table covered twenty-two navigation keys and no letters, so no shortcut inside a
text field could ever fire; and nothing called into the window system to switch text input on,
so on most platforms no character was produced in the first place.

**There was already a broken call site.** The log console's category filter has been a real
text field since M9 and could not receive a single character. It started working in the same
commit as the rename widget, which is the difference between building a feature and repairing
one.

### The three shapes input takes

The milestone's one structural idea is that events, level state and window state answer
different questions and must not be confused.

**Committed text is a stream.** It arrives as its own event, because a key is a position on a
keyboard and a character is what an input method decided the person meant, and neither is
derivable from the other. The bytes are inline — sixty-three of them — because a string would
allocate inside `pump()`, which the header promises it does not, and a view into scratch memory
would dangle the moment a consumer kept an event. The price is that a longer commit arrives as
several consecutive events, each cut on a character boundary, which is lossless because every
consumer appends in order. The cutting is the only part that can be wrong, so it lives in a
free function with no window system anywhere near it and nine tests around it.

**A composition is state.** An input method's in-progress text replaces itself on every
keystroke, so it truncates rather than splitting, and says that it truncated.

**An axis is level.** There is deliberately no axis event: an axis has no transition worth
naming, a resting stick would be the first thing to exhaust the event reserve, and both
consumers poll. Buttons get edges; axes get a value that survives the frame boundary.

### The decisions that were made to be reversed cheaply

**Text input is off unless something is focused.** Always-on is not a smaller amount of code,
it is a different behaviour: with an input method active every key routes through the method,
so the space bar stops pausing the simulation and starts confirming a candidate, and every key
produces a character *and* a key event, so a shortcut key types as well. The overlay already
knows when a field is focused. The application asks it once a frame.

**A gamepad identifier is a slot, not a device.** The window system's own identifier is not
stable across runs, so exposing it would eventually put it in a saved binding. Four slots,
lowest free one on connect, released on disconnect. Face buttons are named by position, because
the button in the south position is "A" on one vendor's pad and "B" on another's, and the two
vendors disagree about which of them means confirm.

**The subsystem is off by default.** Video defaults on because without it there is no
application; a gamepad is a peripheral that every application works without, so a composition
root opts in. Some thirty headless tests therefore enumerate no devices and behave exactly as
they did before.

### One camera, one table

Three character-identical copies of drag-pan and wheel-zoom were each about to grow a gamepad
branch, and M9 had already fixed one bug in four places because of that duplication. That is
the project's own bar for generalising, met three times over, so the arithmetic moved to
`apps/common` behind a plain input struct — which is what makes it testable at all, since only
the platform may write an `InputState`.

**The regression test that never existed now does**: a wheel zoom leaves the world point under
the pointer unchanged. That is the exact property M9's bug broke, and nothing had been asserting
it.

The gamepad's missing pointer turned out not to be a problem. The camera's centre *is* the
world point under the viewport centre, so a stick zoom needs no anchor, while the wheel keeps
zooming about the pointer.

The lab's key switch became a table where a key and the gamepad button that mean the same
action sit on the same row, so the two cannot drift apart. Two new actions, faster and slower,
step a speed ladder and stop at its ends rather than wrapping.

### Exit criteria, against what was done

| Criterion | Status |
|---|---|
| `Event` grows correctly, with the enforcement moving with it | **Met.** Twenty alternatives, twenty names, twenty positional assertions and a new `static_assert` that every alternative is trivially copyable. The last of those was previously a comment describing a property nothing checked, which is what made the inline text buffer a constraint rather than an intention. |
| Typing into a real panel proven end to end | **Met for the automated half.** GPU tests click the log filter through a rectangle the panel reports back, send text, and assert the shown and hidden counts changed; the same recipe renames an entity and asserts one undoable step. **The manual half is outstanding:** no runner has an input method installed, so a real composition committing and a candidate window following the caret have not been observed by anyone. Listed as the first risk in the report rather than waved through. |
| Text input on only while focused | **Met**, in both applications, synced once a frame from the overlay and queried from the window system rather than remembered. |
| A controller pans and zooms with no anchor drift | ~~Met by test, not by hand.~~ **Not met when this was written, and corrected in M12.** Neither application ever enabled the gamepad subsystem, so nothing built here could run in a shipped binary. The platform work was real and remains so; the claim about a person using a controller was not. M12's opening slice turns the subsystem on with the window, adds the integration check that would have caught it, and the report carries the full account. A physical controller has still never been plugged in. |
| What cannot be automated written down as such | **Met, and it is the honest part of this milestone.** Three properties are documented as untestable at the line where someone would delete them: the byte that keeps a character split from reading past the end of a view, the release of held gamepad state on disconnect, and the placement of a candidate list. Three mutation checks survived, and each survival is explained rather than reported as coverage. |
| `precheck` clean, containers green, four workflows green | **Met.** 594 tests on macOS and in the Linux container, 652 including the GPU label, 58 on the software rasteriser with the new overlay tests running rather than skipping, and six CI jobs green on each of the four commits. |

**Corrections this milestone made to earlier work.** A deferral entry claiming the sandbox's
zoom anchor was still broken had been fixed in M9 and never struck. A comment described an
implementation that does not exist. And `event.cpp` stated a property it did not enforce.

**And a correction made to this milestone, hours after it closed.** The gamepad was unreachable
from both applications, and this section said it worked. The gap sat between two kinds of test:
the unit tests each build their own platform, so none could observe what a composition root
asked for, and the integration checks run whole binaries but had no reason to ask what had been
initialised. M12 opens by closing it.

## M12 — Audio

Full report: [reports/M12.md](reports/M12.md).

Slices: a correction to M11; the module, the device and the mixer; the clip asset type and the
WAV reader; measurement; [ADR-0011](adr/0011-audio.md) and the documents.

**Exit criteria**
- A new module with no new dependency, no window-system type in any public header, and an
  error domain that reports as audio rather than as something else.
- A sound generated in code in the lab, and a sound loaded from a file in the sandbox, each at
  a real call site rather than behind a test flag.
- A threading decision recorded as an ADR, with its budget measured at both ends.
- Every golden hash and every headless integration case byte-identical.

### The milestone opened by correcting the last one

M11 reported gamepad support as met, and neither application could use any of it. The subsystem
is off by default and no composition root ever asked for it, so nothing built in M11 for a
gamepad ran in either shipped binary.

**The gap was between two kinds of test rather than inside either.** Every gamepad unit test
builds its own platform and asks for the subsystem itself, which is correct for testing a
platform and is exactly why none of them could observe a composition root that never asked. The
integration checks run whole binaries, where it was visible, but they had no reason to ask what
had been initialised. The platform now logs one line naming every subsystem it brought up, and
an integration case in each application reads it.

### The one decision that was genuinely open

Everything else about audio was decided by the engine as it already stood. SDL's lifetime
belongs to one module, because the platform's destructor shuts every subsystem down at once.
Decoding belongs in `assets`, which links no SDL, so the window system's own loader was
unreachable from where importing happens. A sound cannot reach simulation state, because the
lab's simulation library is fenced at configure time to link nothing else.

What was open was **who mixes, and on which thread**. The window system can drain a stream the
main thread fills, or call into Atlas from a thread it owns. [ADR-0011](adr/0011-audio.md)
chose the first, and the interesting part is not the choice but that its price was measured
rather than asserted: a frame longer than the queued audio is heard as a gap. At a million cells
and thirty ticks a second the worst frame was 19.7 ms and nothing underran. At sixty ticks a
second — already beyond what that tick sustains, by M8's own measurement — one frame reached
70.9 ms and produced exactly one gap, counted rather than swallowed.

### Reading a file nobody can be trusted to have written

The WAV reader is about a hundred and fifty lines and every one of them treats its input as
hostile. The discipline was not invented: the simulation's save reader already has exactly the
right shape and lives in a module `assets` must not depend on, so it was reproduced following
the artifact cache, which is the one hardened reader already in the module.

**Format is chosen by the leading bytes and never by the path's extension**, because an
extension is a claim made by whoever named the file.

Mutation testing found four gaps in the tests written for it, and one of them was a genuine
memory-safety hole: a data chunk claiming ten thousand bytes in a file holding a hundred read
past the end of the buffer while every existing test still passed, because the obvious
four-billion-byte case is caught by a different check.

### Exit criteria, against what was done

| Criterion | Status |
|---|---|
| A new module, no new dependency, no leaked types | **Met.** `atlas::audio`, deps `core;assets;platform`, SDL private. Nothing was added to `vcpkg.json`; the Ogg decoder the package already installs is deliberately not compiled. An error code numbered 500 used to report itself as a serialization error, because the domain ladder had no ceiling on its top rung; it has an audio rung now and both sides of every boundary are pinned. |
| Sounds at real call sites | **Met.** The lab clicks where a person picked a cell, panned by where they clicked, and on a save and a load; its click is generated because the lab has no asset registry and deliberately does not want one. The sandbox plays a loop read from a file and picks up an edit to it while playing, with the clip count proving the displaced clip was released rather than leaked. |
| A recorded threading decision, measured | **Met.** ADR-0011, accepted, with the callback mixer recorded as its rollback and a latency complaint above about 80 ms as the trigger. Measured at both ends of its own budget, including the configuration where it audibly fails. |
| Determinism untouched | **Met.** Every golden hash and every headless integration case byte-identical. Structural rather than promised: an audio include in `simulation` fails the boundary check, and the lab's simulation library cannot link the module at all. |
| Predictions before measurements | **Met, and two of three were wrong.** Thirty-two voices cost 30.7 to 31.4 µs against a predicted 10 to 30, and one voice 1.08 to 1.12 against a predicted "under 1". Recorded as low rather than widened after the fact. No baseline was recorded at all, because the machine was not idle and recording writes the whole file. |
| Music | **Not built, by owner decision**, with a written trigger. The decoder is installed on every platform at no cost; what M12 declined was the repository's first committed binary test fixture, which an Ogg forces and a WAV does not. |

**Corrections this milestone made to earlier work.** An asset type has been able to decode and
then wait forever since M4, with nothing logging and nothing failing. The error-domain ladder
was open-ended at the top. The rule naming where SDL types may live named three places and there
were four. The module diagram called the handle pool a slot map. And no committed binary asset
had any recorded provenance, which is now a file that records, among other things, that nobody
knows where `tile.png` came from.

## M13 — Animation

Full report: [reports/M13.md](reports/M13.md).

Slices: a sweep and the demonstration's first test; the pose in isolation; the module;
[ADR-0012](adr/0012-scene-format-v2.md) and scene format v2; [ADR-0013](adr/0013-animation-clip-format.md)
and the clip file; rotation in the renderer; the consumer; the documents.

**Exit criteria**
- A clip read from a file moves and turns an entity, and cycles another through a sheet.
- An entity can be edited while it plays, and undo takes back the edit and not the playback.
- The scene format reaches version 2, and the migration rule is asserted rather than assumed.
- The rotated-sprites deferral fires, measured against a prediction written first.
- The hand-written animation in the sandbox is deleted, not merely bypassed.

### The milestone existed because its consumer was broken

Most milestones here are built against a consumer that works and would be better. This one was
built against a consumer that had a dilemma with no good half, and had had it since M5.

`SceneDemo::tick` wrote the sprite-bearing roots straight through the scene pointer every tick.
Its own comment admitted the cost: editing one of those while the animation ran was pointless,
because the next tick overwrote it. **Pausing did not rescue the editor either, and that part was
written down nowhere.** `tick` returned before it reached `update_transforms`, and the edit
history deliberately does not recompose — it bumps a revision and leaves that to whoever is
watching, and nobody was. So while paused, dragging an entity changed the authored number,
changed what the inspector displayed, and never moved the picture. The Space key existed to make
the editor usable on this scene and did not.

**The inspector had been displaying the contradiction the whole time.** It shows a local position
beside a world translation, and the comment beside that row says it exists so a reader can see
which of the two disagrees. It was doing its job; nobody had followed it back to the cause.

### The decision that dissolved it rather than picking a side

`CLAUDE.md` said an application must not animate what the user can edit. That rule was honest
about a real hazard and it made the editor useless on the one scene it exists to edit.

[ADR-0012](adr/0012-scene-format-v2.md) replaces it. Authored components belong to the history;
derived ones belong to whoever computes them; **neither writes the other's fields.** The animator
writes `AnimationPose`, which is derived exactly as `WorldTransform` is — recomputed, never
authored, never saved — and composition adds position, adds rotation and **multiplies** scale, so
an untouched pose is the identity in all three channels. An additive scale would shrink
everything to nothing, silently and totally, which is why that one is not a preference.

The change of mind is recorded in an ADR rather than made quietly in the rules file, which is the
project's standing rule about changes of mind and the largest application of it since ADR-0010.

### A format version that owed no migration, and the rule that says so

The scene format reached version 2 when it gained an animator. ADR-0007 had anticipated exactly
this and declined to write a migration, on the grounds that a migration with nothing to migrate is
untested by construction.

The answer turned out to be that there is still nothing to write, and that this is a rule rather
than a reprieve: **a version that only appends components is read by accepting a range of versions
and branching on none of them.** A component is read when its key is present and absent when it is
not, and a version 1 writer never produced the key, so an older file is already a valid newer one
with some components missing.

The rule is asserted literally: a version 1 document loads, re-saves, and equals itself with only
the version number changed. A change that is *not* a pure append fails that test, which is the
test telling whoever made it that they now owe a migration.

### Continuous integration caught something nobody introduced

The rotation slice failed CI reporting the committed sprite shader as stale when it was current.

The shader currency check compares every cooked output byte for byte against a fresh build. That
is only meaningful when the same compiler produced both, and **it never was**: this machine cooks
with Homebrew's glslang and CI with the distribution's package, and `DEPENDENCIES.md` has recorded
two different versions since M2. Every shader until M13 was simple enough that both compilers
produced identical bytes, so the check had been passing by coincidence for six milestones. The
first shader with real arithmetic in it turned that coincidence into a false failure.

The comparison now happens where it means something and is skipped, loudly and with both toolchain
strings printed, where it does not. The lesson generalised into a rule: **a `--check` that compares
bytes must compare bytes only this repository decides.** The sprite sheet committed a slice later
stores its pixel data as uncompressed deflate blocks for exactly that reason — a PNG's pixels are a
zlib stream, and zlib's compressor is free to change its output between versions.

### A measurement that was measuring luck

The rotated-sprites deferral fired on its own stated trigger, the first consumer needing a rotated
instance. The quad instance grew from 48 bytes to 64, carrying a rotation and a normalised pivot,
with the trigonometry left on the graphics device.

The first measurement said 2.9× at a hundred thousand quads against a prediction of 1.15× to
1.35×. **The measurement was wrong, not the change.** The two sets were taken twenty minutes apart
on a machine that was not idle; the "before" minimum was a lucky run and the "after" set never got
one, so comparing minima compared the best luck each set happened to have. Choosing the minimum had
seemed like the careful option and it made the error larger.

Measured properly — both binaries built, a cooked shader directory kept for each, the two
alternated in blocks within the same few minutes — it is **1.20× at a hundred thousand and 1.30× at
ten thousand**, with allocations per frame unmoved. The larger prediction was right and the smaller
slightly low.

**What caught it was the prediction's own threshold**, written down in advance: anything past 1.35×
meant finding out why before reporting. Without that line, 2.9× would have been reported as a cost
rather than investigated as an anomaly.

### Exit criteria, against what was done

| Criterion | Status |
|---|---|
| Clips that move, turn and cycle | **Met.** The parent root plays an ellipse and one full revolution from `orbit.clip.json`; because every other sprite hangs off it, one revolution turns nine quads, so the renderer's new path runs on an ordinary frame rather than only in a test. A grandchild cycles four cells of a generated sheet from `cycle.clip.json`. |
| Editing while playing | **Met**, and asserted end to end. `--anim-check` drags the animated root after 240 frames of playback, advances another frame on top of the edit, and checks the root is drawn where its authored position and its pose add up to; then undoes, and checks the authored value is restored exactly and the animation untouched. |
| Scene format v2 and the migration rule | **Met, with no migration code**, which ADR-0012 establishes as the rule for a pure append. The property is asserted rather than described. |
| Rotation, measured | **Met.** 1.20× and 1.30× against a prediction committed before the change. The first measurement was thrown away for comparing minima taken twenty minutes apart, and the report says so rather than quietly reporting the second. |
| The hand-written animation deleted | **Met.** `tick`, `m_animating`, the Space key and the rotation warning are gone. Two behaviour changes come with that and are named in the report rather than left to be found: pausing no longer makes the sprite jump, and an animated entity can now keep an authored rotation. |
| Determinism untouched | **Met.** Every golden hash and every headless integration case byte-identical, structurally rather than by promise: `animation` cannot link `simulation`, the pose is not serialised, and the lab's simulation library is fenced at configure time. |
| Whole-program proof | **Met.** `--anim-check` runs the committed clip files through the real asset pipeline — requested by virtual path, parsed on a worker, finalised on the main thread, played — and compares poses against a table recorded from the files by working their arithmetic out separately. To a tolerance, because float easing contracts differently between compilers; the clock is integer and is compared exactly. |
| Per-sprite texture binding | **Met in the application, not in a test.** The draw path binds per sprite so the grandchild can use the sheet. No headless check has a device and there is no golden-image harness here, so it was confirmed by rendering the scene and examining the image. Recorded as a risk. |

**Corrections this milestone made to earlier work.** The shader currency check had been comparing
two compilers' output since M2. The vertex attribute offsets were hand-written literals. The shader
cooker collected reflection it never compared against anything. The demonstration scene had no
automated coverage of any kind. The boundary checker could not see the JSON reader, in the
milestone that gave it a second consumer. `alpha` was documented as driving interpolation and drives
a headless sleep. The rotation warning misreported its own scope. `m_orbiting` was dead. Two
serialization tests were testing indentation. And `tile.png`, committed in M4 with no recorded
origin, is generated now — `PROVENANCE.md` has no unexplained files left in it.

## M14 — Networking: lockstep design and a loopback proof

Full report: [reports/M14.md](reports/M14.md).

Slices: a sweep; [ADR-0014](adr/0014-deterministic-lockstep.md) and the module's shape; the turn
gate; three extractions; the gate reaching the kernel; the wire protocol; the inbox and the link;
the session; the two-kernel proof; the lab and its integration cases; the benchmark and the
documents.

**Exit criteria**
- A design record accepted before anything is built on it.
- A tick that cannot run until every participant has said what it is doing, with readiness
  depending on who has reported and never on elapsed time.
- Several simulations in one process agreeing hash for hash through the real peer interface,
  under latency, reordering and injected faults.
- No transport, nothing added to `vcpkg.json`, and every golden hash unchanged.

### The groundwork was already there, which is why this was affordable

The command queue's own file comment has named a network peer as one of its four callers since
M6. Its ordering rule — sort by `(source, sequence)`, never by arrival — was documented as *"a
total order that two machines can agree on without agreeing on timing"*. `SourceId` was described
as *"an index, not an address: it is written to replays and compared across machines"*.
`submit_stamped` already existed as the receive path. Saves already persisted per-source sequence
counters. None of that was built for networking; all of it was built because determinism demanded
it, which is why lockstep is the shape that fits and why it costs kilobytes a second rather than
megabytes.

**What was missing was one thing: nothing waited for anybody.** `Kernel::step` ran whenever its
schedule was finalised, and there was no concept anywhere in the engine of a tick that was not
yet allowed to happen.

### The decision that carries the milestone

**Readiness depends only on which sources have reported. Never on time.** The gate reads no
clock, holds no deadline and has no timeout — and a timeout is the obvious thing to reach for,
which is why its absence is written down rather than left to be noticed. The moment readiness
could turn on elapsed time, two machines at different frame rates would run different ticks with
different commands, which is exactly the failure that stamping a command with its target tick was
introduced to prevent.

**The kernel refuses a tick the gate has not cleared**, rather than trusting its caller to ask.
That widened a documented contract — `step` used to fail only when the setup was wrong — and the
change is recorded in the ADR rather than quietly outgrown. The alternative keeps the contract
and lets a composition root that forgets the check diverge silently; M11 shipped a gamepad no
composition root ever enabled, and the lesson taken from that is to make the omission loud.

**The refusal sits before the command drain**, and that is the single most consequential line.
`drain` *removes* what it returns, so a tick refused after it has already discarded its commands;
the retry would run with fewer and reach a different state from every peer, silently. The test
that pins it submits a command, refuses the tick, and checks the command is still there when the
turn arrives.

### What the proof actually proves

Two kernels over a link with latency and reordering, compared **at every tick** rather than at
the end — a run that diverged and reconverged diverged. Both peers claim the same cell every tick
with different claimants, so the total order decides the winner, and the refusal is itself
recorded in the state: a rejection that were a silent no-op would be indistinguishable from the
command never arriving.

That last point was learned the hard way. Breaking the total order — sorting by sequence and
ignoring the source — survived the entire proof at first, because each peer picked its own cell
and the two commands commuted. Only a tick where the order decides the outcome can show that the
order is agreed.

And the whole-program proof: `atlas_lab --loopback-peers 3` runs three simulations, and the
binary itself refuses to exit zero unless they agree. A script comparing two printed numbers is a
fine second opinion; the binary refusing is what makes the property hold for every run anybody
ever does.

### Exit criteria, against what was done

| Criterion | Status |
|---|---|
| A record accepted before the work | **Met.** ADR-0014, accepted at the milestone's one gate, carrying the amendment to the kernel's failure contract. |
| A gate with no clock | **Met.** No timeout, no deadline, and a test that asks ten thousand times and gets the same answer. A silent peer costs one small record for ever, checked rather than claimed. |
| Agreement under an unkind link | **Met.** Two kernels at every tick over latency and reordering; three peers in the lab under the same, plus injected drops, holds and corruption. |
| A solo run unchanged | **Met.** Every golden hash byte-identical, and the fixed scenario now runs with a gate that expects nobody — the only case that proves none of this changed the simulation, because every other compares two runs that move together. |
| No transport | **Met.** Nothing in `vcpkg.json`, no socket, and the criteria for choosing one recorded rather than resolved. |
| Measured against a prediction written first | **Met, and the prediction was high.** 1.4 µs per peer per tick against a predicted 2–6. Recorded as high rather than widened. |

**Corrections this milestone made to earlier work.** The replay writer could produce a recording
its own reader refused — written successfully, permanently unloadable — found while surveying the
file the command codec was lifted out of. `read_header` did not validate the hash algorithm
version, which mattered because the handshake is exactly that kind of cheap peek. The replay
refused an older recording with the same terse message as a newer one. Divergence attribution
reported "no per-system hashes were recorded" even when hashes were recorded and all matched.
`TickReport` counted a late command and an invalid one as one number. And `SourceId::Local` was
hard-coded at two lab call sites, where it would have meant one peer signing another's name to
its own commands.

**A design this milestone built and then removed.** Slice 4 gave a command source a handle bound
to one identifier, so that "mark only your own turn" was unrepresentable rather than documented.
The first real implementation wanted the opposite: a session speaks for every peer it is
connected to. It was built for a consumer that did not exist, removed three slices later, and
recorded in `DEFERRED.md` with M15's mod host as the trigger.

## M15 — Sandboxed mods

Full report: [reports/M15.md](reports/M15.md).

Slices: a sweep; [ADR-0015](adr/0015-sandboxed-mods.md) and the runtime evaluation; the overlay
port, the runtime and the loader; the guest interface and the mod host; the lab consumer and the
demonstration mod; the three proofs; the benchmark and the documents.

**Exit criteria**
- A runtime chosen against evidence, at a gate, before anything reached `vcpkg.json`.
- A sandbox that survives modules written to break it, under the address sanitiser.
- A mod that reaches simulation state only through the command queue, under its own identifier,
  with no way to claim another.
- A recording made with a mod replaying on a build that never loads one.
- Two peers running the same mod agreeing hash for hash without exchanging its commands.
- A mod that reads a clock refused, and divergent when it is not.

| Criterion | Status |
|---|---|
| A record accepted before the dependency | **Met.** ADR-0015 written and stopped at, then the port at a second gate. The evaluation became three-way when the baseline turned out to hold `luau`, which the planning conversation had not known. |
| A sandbox that holds | **Met.** Sixteen hostile modules including a truncation sweep over every prefix of a valid one, all under ASan. Two WAMR APIs that report memory limits wrongly are bypassed by reading the module's own bytes, because a limit that silently does not apply is worse than none. |
| A mod cannot be another source | **Met, and unrepresentable rather than forbidden.** `atlas_submit` has no source parameter. Mod identifiers carry bit 31 and cannot arrive over a link. |
| Replay needs no sandbox | **Met.** ADR-0009 decision 2 paying off with no code: a recording made with a mod replays to the same hash on a run that starts no runtime. |
| Peers agree | **Met.** Two peers under latency and reordering, with the anti-vacuity half in the same case — the run without the mod must end somewhere else. |
| No clock, enforced | **Met.** Refused without `--unsafe-debug-imports`; with it, divergence at tick 16 attributed to a system. That case fails if a clock is ever added for real. |
| Measured against a prediction written first | **Met, and the prediction was wrong.** 0.21 µs per mod per tick against a predicted 1–4, and loading four times faster than its lower bound. Recorded as wrong rather than widened. |

### What the dependency actually cost

The only milestone in the series to add one, and the cost was not where the record expected it.
Three continuous-integration failures before the dependency did anything useful — a configure
race on a source tree vcpkg builds twice in parallel, `dllimport` on a static library, and an
unreachable loop MSVC was right about — **all but one in the port, and none reproducible on the
machine that wrote them**. Then nothing: four slices of work on top of it, every lane green first
try. The tax looks per-version rather than per-change, which is the shape worth knowing before
the next WAMR bump.


## M16 — Localisation: string tables, English only

Full report: [reports/M16.md](reports/M16.md).

Slices: correcting the record; [ADR-0016](adr/0016-string-tables.md) at a gate; the `text`
module; the asset type, its parser and its finaliser; routing everything the overlay shows;
the proofs, the benchmark and the documents.

**Exit criteria**
- The record corrected, because the consumer this milestone was scheduled against did not exist.
- A record accepted at a gate, answering ADR-0010 D6 rather than inheriting a justification.
- A module with one lookup and one substituter, and no exception allow-list entry.
- An asset type that cannot sit unclaimed, because its finaliser landed with it.
- Every key the interface can ask for resolving against the shipped table, proved rather than
  asserted.
- Measured against a prediction written first.

| Criterion | Status |
|---|---|
| The record corrected | **Met, and this was the milestone's first finding.** M15's report claimed it supplied this milestone's consumer; its ABI has eight imports and none touches a string table. ADR-0010 D6 makes "drop it" a legitimate outcome, so the accounting went to the owner before anything was built. |
| A record at a gate | **Met.** ADR-0016 written and stopped at, carrying the D6 accounting, the refusal of `std::vformat` with its enforcement, and the glyph limit. |
| One lookup, one substituter | **Met.** `engine/text` has one module consumer, `tools`, and needs no allow-list entry: the substituter is hand-written precisely so it cannot throw. |
| A type that cannot sit unclaimed | **Met.** `AssetType::StringTable = 5` and `Catalog::finalise_pending` in one commit, with an integration case asserting `awaiting_finalisation` is zero. |
| Every key resolves | **Met.** `--text-check` walks all 130 keys; the integration case removes an entry and asserts it fails and names it, because a missing key deliberately renders as itself and a check that could not fail would look identical to one that worked. |
| Measured against a prediction written first | **Met, and both predictions were wrong.** 7.2 ns a lookup against 20–60, and 57 ns a substitution against 100–300. Recorded as wrong rather than widened. |

### What the milestone was actually for

Its stated consumer did not exist, and the owner chose to build with that known. What replaced
the justification was duplication that did: **three copies of the log severity words and two of
`speed_name`**. The second `speed_name` was not a copy — it was missing the fractional-speed
check, so the lab reported a half-speed run as "1x" on the one row that says how fast time is
running. That bug had been there since M7 and nothing had noticed, because a second spelling of
a rule is exactly where a discrepancy hides.

### The honest limit

**A second language is a data change only for Latin-1.** The overlay renders Dear ImGui's
default atlas, so French, German, Spanish and Italian would work from a translated file and
Polish, Greek, Russian and every CJK language would show blanks. Recorded before any of the
milestone was built, rather than discovered after somebody shipped a Polish table.

## M17 — A transport for lockstep

Full report: [reports/M17.md](reports/M17.md).

Slices: the sweep; [ADR-0017](adr/0017-lockstep-transport.md) at a gate; the `net::Link` seam;
the dependency and the hub at a second gate; the lab and two processes; the benchmark and the
documents.

**Exit criteria**
- A legal message can be received, and the two bounds that disagreed become one rule.
- A record accepted at a gate, answering ADR-0014's five criteria one at a time.
- A transport behind an interface, with the loopback tests unchanged as the proof of the seam.
- Two processes agreeing hash for hash over a socket, and a killed peer ending the session
  rather than hanging.
- No new thread, and ARCHITECTURE's "No new thread" sentence still true and unedited.
- Measured against a prediction written first.

| Criterion | Status |
|---|---|
| The bounds reconciled | **Met.** `kMaxMessageBytes` was 8 MiB against an inbox budget of 1 MiB, so every message between them was legal to send and impossible to receive. A `static_assert` ties them now. Planning found a second disagreement and it is recorded rather than fixed: the command count times the payload ceiling permits a 256 MiB turn, so the *message cap* binds a turn's size and not the counter. |
| A record at a gate | **Met, and two of three candidates were eliminated by criteria this project wrote down before it knew they existed.** GameNetworkingSockets carries `"supports": "!uwp & !(arm64 & windows)"`; SDL3_net is a socket wrapper rather than a reliable ordered channel, and the only candidate needing a `net → platform` edge. |
| The seam | **Met.** `net::Link` is four calls. Every existing net test passed unchanged — none of them names `LinkEnd` — and `session.hpp` no longer includes the loopback at all. |
| Two processes agree | **Met.** Both reach the same hash at the same tick over a real socket, with the anti-vacuity half in its own case: a pair must not reach a solo run's hash. Verified the case can fail by making the hub drop what it receives. |
| A killed peer | **Met, and it needed two mechanisms.** A hub says goodbye in its destructor; a process killed outright cannot, so the transport's deadline notices instead. The first draft had only the graceful case and it failed, which is how the gap was found. |
| No new thread | **Met.** ARCHITECTURE's sentence and its threading table are both unedited, and `net`'s module edges are still `core;simulation` — ENet links `PRIVATE`. |
| Measured against a prediction written first | **Met, and for once the prediction held.** 22.4 µs against a predicted 20–100, after four wrong ones in three milestones. |

### What choosing ENet actually bought

The clearest measure is what did **not** change. `cmake/ModuleGraph.cmake` is untouched; so is
the mermaid diagram; so are CLAUDE.md's SDL rules. SDL3_net looked cheapest in the manifest —
one already-pinned dependency — and would have cost a `net → platform` edge that the module
table forbids by name, cascading into six places. The module that is pure state exchange would
have acquired the engine's heaviest presentation dependency in order to obtain a socket.

### The deadline that does not break determinism

A quiet peer ends the session, and the deadline for deciding it lives in the transport. The turn
gate still reads no clock, so ADR-0014's central invariant is intact rather than merely
respected. The alternative — dropping a peer and continuing — is deferred with its reason:
every remaining peer would have to apply the drop at the identical tick or diverge, which needs
an agreement protocol whose subtle failure is the exact thing lockstep exists to prevent.

## M18 — Chess: the record, and M16's hotfix

Full report: [reports/M18.md](reports/M18.md).

Slices: the overlay showing words rather than keys, with a test that counts lookups;
[ADR-0018](adr/0018-chess-probe.md) at a gate.

**Exit criteria**
- The M16 regression fixed, with a test that would have caught it.
- A record deciding where chess lives, what it can and cannot prove, and which engine changes
  it forces — each of those by its own later record.
- The `runtime` deferral answered rather than left firing.

| Criterion | Status |
|---|---|
| The regression fixed | **Met.** Five titles, every statistic label, the mode buttons and two lab values now resolve. `Catalog::lookups()` and `test_stats_panel.cpp` assert one lookup per key handed to the panel — `misses() == 0` was true of the broken overlay too. Three mutation checks, one of which survived a first draft of the test and is written up. |
| A record at a gate | **Met.** ADR-0018: in-tree by exception, not v1.0 by the charter's own letter, fenced to `atlas::simulation`, the two engine changes decided by 0019 and 0020 before the code that needs them. Nine predictions scored: seven confirmed, one half, one wrong in mechanism, three findings unpredicted. |
| The `runtime` deferral | **Met.** Not built; count at three; condition sharpened to the loops converging or a fourth application. |

### What the predictions were for

The plan wrote nine claims about where the engine would bend **before** surveying the code,
so that they could be wrong. Seven were right, which is less interesting than the three things
the surveys found that nobody predicted — one of them a regression on `main` that the milestone
it belonged to had, in its own report, named as a class of gap and not looked for. A consumer
finds what a checklist cannot, and that is the argument for building one.

## M19 — A command may be declined

Full report: [reports/M19.md](reports/M19.md).

Slices: [ADR-0019](adr/0019-declined-commands.md) at a gate; the mechanical migration with
every golden hash unchanged; the three hidden refusals in the tree becoming real declines, each
in its own commit; the documents.

**Exit criteria**
- A record accepted at a gate, deciding the third fate of a command and why it is not a
  rejection.
- `apply` receives its context and returns a verdict; twelve handlers migrated; every golden
  hash byte-identical.
- The lab, the synthetic scenario and the net harness each decline where they used to return
  silently, with the "changed nothing" contract tested against a twin.
- The lockstep proof M14 could not write: two peers agreeing on a decline with nothing in the
  state to show for it.

| Criterion | Status |
|---|---|
| A record at a gate | **Met.** ADR-0019, Proposed in the morning and Accepted in the afternoon of 2026-09-23, with one decision corrected by implementing it and marked as such. |
| The migration | **Met.** Twelve lambdas, not the eleven the plan counted. Both engine goldens and the lab's golden byte-identical before anything declined, which was the slice's only exit criterion. |
| Three real declines | **Met, and the lab's found a bug.** Its hidden refusal was unreachable — the queue re-validates an instant before `apply` — and what it never checked was the table it indexed, so a bound that lagged a load would have written past the end. It declines now. Five mutations, each caught; one test's arithmetic was wrong before the kernel was. |
| The proof M14 could not write | **Met.** Both peers claim the same cell every tick with a handler that declines rather than records; most of the run is declines, the counts and hashes agree tick by tick, and the contest counters the recording fixture uses stay at zero. |

### What the third fate is for

Chess needs it sixty times a game, but the argument for it was already in the tree: M14's
proof of a state-dependent refusal had to *write the refusal into the world* to make it
observable, and its own comment said why. A refusal the kernel cannot see is one it counts as
an application, records as one, and hashes as one. The change is one field on the report and
one parameter on the handler; what it buys is that the lockstep proof can now assert the thing
it was written to assert.

## M20 — Chess: the rules library

Full report: [reports/M20.md](reports/M20.md).

Slices: the fence and the five tables; the position, movement, notation and perft; the command,
its declines and the six endings; the Opera Game as the third golden.

**Exit criteria**
- A rules library fenced to `atlas::simulation`, with every rule including the draws.
- Perft against published counts on six positions.
- A famous game's final position as a golden hash, replayed and saved through the engine's own
  paths with nothing chess-shaped in either.
- No engine change.

| Criterion | Status |
|---|---|
| The fence | **Met, and proved before it was committed.** A platform link added to the library's dependencies fails configure with the fence's message. |
| Every rule | **Met.** Castling with its three conditions, en passant offered only when a pawn can take, four promotions, rights spent three ways, check, mate, stalemate, the fifty-move rule, threefold on a key that ignores the clocks, insufficient material without over-recognising two knights. |
| Perft | **Met on the first run.** Six positions, every shallower depth too so two errors cannot cancel. The tests were wrong twice before the generator was once: a pinned bishop asked to move along its pin, and an expected FEN with the castling field from memory. |
| The golden | **Met.** Thirty-three plies to mate, final FEN asserted, three hashes recorded on macOS arm64 for the other lanes to confirm. Replayed with every checkpoint matching; saved at ply sixteen and finished elsewhere to the same hash. |
| No engine change | **Met.** M20 touched nothing under `engine/`. Prediction 8 — that save, replay and the hash would need nothing — was tested for real here and held. |

### What the probe found this time

Nothing in the engine, which is the finding. The rules are 1,900 lines of library and 1,300 of
tests, they link one module, and the only engine facility they needed that did not exist in M17
was the one M19 built for them. The state fits the world's table contract without a wrinkle,
the empty schedule ticks, and the replay and save paths carried a real game the way they carry a
synthetic one. The hypothesis that a map engine's simulation generalises to a board game has
now been tested with the game itself rather than with a survey, and it held.

## M21 — A session can finish

Full report: [reports/M21.md](reports/M21.md).

Slices: [ADR-0020](adr/0020-session-finish.md) at a gate; the finish message and protocol
version 2; the two states and the finished condition; the lab finishing in both modes; a fix to
where a mismatched run is named; the documents.

**Exit criteria**
- A session that ends because its run is over, with both peers exiting zero.
- The condition decided from messages, with no clock, and every clause tested by removing it.
- Two peers told to run different lengths both fail, and say why.
- M17's end-of-run window closed rather than narrowed.

| Criterion | Status |
|---|---|
| A clean ending | **Met.** Both socket processes and every loopback peer log an agreed finish at the bound and exit zero. |
| The condition | **Met, with one clause fewer than the record proposed.** "Every partner's turns have arrived" is "this peer has run the last tick" in other words, and is written once. Seven session mutations and four lab mutations caught — three after fixes to the tests, each written up. |
| A mismatch named | **Met, and it took a second attempt.** The first version passed because of a timing, not a guarantee; a mutation removing the fix survived three runs. The application compares a partner's declared finish with its own bound, and the case runs at the input delay where the stall is the usual timing. |
| The window closed | **Met.** A partner leaves only once it is finished, which needs this peer's finish first, so a hang-up after it carries nothing — and the lab asks `finished()` before it asks whether the link ended. |

### What the second engine change found

The record was wrong twice and a test found both. One clause of the finished condition was
another clause restated, and a test built to fail without it could not, because the other one
masked it. And the record's claim that the transport's deadline covers a partner that never
finishes was false for the one case that mattered: a partner told to run longer is connected,
answering keep-alives, and waiting. Only the application knows how long a run was meant to be,
which is why the fix lives there. Both corrections are dated in the record rather than edited
into it.

## M22 — Chess: the application, two people, a socket

Full report: [reports/M22.md](reports/M22.md).

Slices: the board, playable hot-seat, with the integration helpers lifted into a shared file;
the interface's text, with `Catalog::add_table`; two people over a socket, with the session
code lifted into `atlas::app_lockstep`; the benchmark with its prediction committed first, and
the documents.

**Exit criteria**
- A game two people can play in a window, drawn from a generated sheet, with nothing chess in
  `engine/`.
- Its text from its own table, checked by `--text-check`.
- Two processes playing a game to checkmate over a socket, both exiting zero.
- Measured against a prediction written first.

| Criterion | Status |
|---|---|
| Playable | **Met.** Click a piece and its legal moves show; the move is a command through the kernel. Checked by eye on this machine's GPU, and ten integration cases play real games from the command line. |
| Its own text | **Met.** Seventeen keys in the application's header, a table beside the engine's, and `--text-check` failing by name when the table is dropped. The one engine change was `Catalog::add_table`. |
| A socket game to mate | **Met.** Two processes play Scholar's mate knowing only their own moves; both reach the same position and hash and finish the session at the same tick. A partner who leaves mid-game and two sides set up differently both fail as they should. |
| Measured | **Met, and wrong by an order of magnitude.** Perft to depth three in 172 µs against 1–5 ms; a ply in 1.25 µs against 10–40 µs. |

### What the probe found this milestone

Two engine facilities chess needed and did not have: a way to add an application's string table
beside the engine's, and nothing else. The session code the lab had written for itself turned
out to be exactly what chess needed, and it moved into a shared library rather than being copied;
the move found a port parser that had accepted trailing characters. A branch copied from the lab
— comparing a partner's declared finish with this side's idea of the game — turned out to be
unreachable for chess, where the only way two games differ is a divergence the session already
refuses, and it was removed rather than kept as decoration.

## M23 — One pin for vcpkg

Full report: [reports/M23.md](reports/M23.md).

The first of the three items M17 left open, taken in the order the owner chose on 2026-09-24:
this, then a string API for mods (M24), then dropping a peer and playing on (M25), one milestone
each.

**Exit criteria**
- The binary cache key cannot follow one vcpkg pin while the build uses another.
- The fix checked where every other check runs, and shown to fail when it should.

| Criterion | Status |
|---|---|
| The key follows the pin | **Met, by removing the possibility of two pins rather than by hashing both.** The key already hashes `vcpkg.json`; `tools/check_vcpkg_pin.py` holds its baseline equal to the submodule's gitlink, and to the commit `DEPENDENCIES.md` names, so the key cannot follow one while the checkout is the other. |
| Checked everywhere | **Met.** A lint test, a precheck step, and a step in the lint workflow. Moving each of the three records alone fails the check, the gitlink tested by staging a different commit in the index. |

## M24 — A mod speaks

Full report: [reports/M24.md](reports/M24.md).

The second of the three items M17 left open. Decided by
[ADR-0021](adr/0021-mod-messages.md): a mod may name a message for a person to read, in its own
namespace, and nothing comes back. The trigger had not fired, and the record says so first.

**Exit criteria**
- A mod can put a message in front of a person without being able to read any text itself.
- A mod's words come from its own table and cannot reach anybody else's.
- Saying something changes nothing a hash can see.
- Every refusal the record names is exercised through a real guest.

| Criterion | Status |
|---|---|
| One way, by key | **Met.** `atlas_say` takes a key suffix and integers and returns a status. No import returns text, a length, or whether a key exists. |
| Its own words only | **Met twice.** The host prefixes every key, so naming another's is unrepresentable; a table naming anything outside the prefix is refused whole, shown by a case that uses a key nothing else defines, so the catalogue's own clash check cannot be what catches it. |
| Presentation only | **Met.** The herald's run ends at the hash of a run with no mod. |
| Refusals through a guest | **Met.** Malformed key, too many arguments, budget, queue bound and an out-of-bounds argument pointer, each in a module built in the test. |
| A panel in the lab | **Not built, by a dated change to D5.** The log console shows messages in a window; a second list was not worth one demonstration mod. |

## First continuous integration

A remote was created on 2026-09-15 and the four workflows ran for the first time. Everything
before that date was verified on one machine.

It took seven attempts and found **twelve distinct problems**, none of which any local check
could have found. They fall into three groups.

**Things only a different machine could show.** The submodule was cloned shallow, which broke
every workflow. Windows had no build tool on its path. vcpkg needed autotools and then a
development header that the arm64 container never pulls, because it never builds that port.
The sanitizer runtimes are a separate package on Ubuntu, so the compiler accepted the flag and
then failed to link.

**Things only a different compiler could show.** A test aliased the logging namespace to
`log`, which collides with the mathematics function that MSVC's headers pull into the global
namespace. MSVC deprecates standard C functions in favour of its own. And it found a shift by
the full width of its own type in the hasher: well defined, correct in every value it ever
produced, and reading exactly like a bug.

**Things only a configuration nobody had run could show.** Release builds were broken on both
macOS and Linux. The debug log macros expanded to nothing, discarding their arguments, so any
symbol used only in debug logging became unused, which is a warning, which is an error here.
The macros now type-check their arguments in a discarded branch that emits nothing. A mistake
inside a debug log statement now also fails to compile in release, which is the right way
round for a mistake to be found.

Separately, clang-tidy had never analysed the scene or simulation modules, because it is
opt-in locally and this workflow had never run. Forty findings across eleven checks, and the
workflow was pinning a different clang-format major from the one the script requires.

**What is now verified on every push:** macOS arm64 and Linux x86_64 in Debug and Release,
Windows x64 in Debug and Release, address and thread sanitizers, the Vulkan renderer on a
software rasteriser, formatting, module boundaries, licence headers, shader currency, and
static analysis.

**What is still not verified:** Windows has no graphics path coverage, because hosted runners
have no graphics hardware and the software rasteriser lane is Linux. The Direct3D 12 backend
is not built at all ([ADR-0006](adr/0006-shader-toolchain.md)). No real non-Apple graphics
hardware has ever run this code.

## Risks and deferred work

Everything consciously not built is listed, with its reason and what would change the
decision, in [DEFERRED.md](DEFERRED.md). Top risks, as of 2026-09-17:

- **The renderer is verified on one graphics processor and one software rasteriser.** The
  llvmpipe check covers Vulkan; Direct3D 12 remains unbuilt and unverified, and no real
  non-Apple hardware has ever run this code. This is now the largest risk in the project.
- **No local Windows machine.** Windows is compiled and tested on every push, which is what
  retired the risk that used to lead this list, but nobody has ever watched the sandbox or the
  lab draw a frame there. A Windows failure that a test does not express would not be seen.
  The plan asked for a decision by M3 on hardware or a virtual machine; it is still open.
- Cross-architecture float divergence between arm64 and x86_64 (M6); mitigated by
  integer-first authoritative state and `-ffp-contract=off` from M0. **Retired as a risk for
  the golden scenario**, which agrees across arm64 and x86_64 and on Windows, and remains open
  for any float that enters authoritative state later.
- **Continuous integration depends on the repository staying public.** The billing limit for
  private repositories was reached in M8 at about 103 billed minutes a push, mostly from the
  macOS multiplier. Going public removed the meter. A future decision to make it private again
  brings the whole risk back, and the fix would be to drop lanes rather than to pay per push.

Retired since the last review: "nothing has ever been verified anywhere but this machine",
which four workflows over six jobs on three platforms have answered, and snapshot copy cost
growth, which M8 measured and found not worth pooling.
