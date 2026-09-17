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
| M7 | Strategy Lab (engine v0.1) | L | next |
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
- **No command or undo infrastructure, and so no editing.** The scene panel takes the scene by
  const reference, which means the compiler enforces the restriction rather than discipline
  doing it. Mutation arrives in M9 together with the infrastructure that makes every change go
  through one validated path; adding widgets first would create a second way into the scene
  that bypasses the checks `Scene` performs.

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
- **Worker-count invariance belongs to M8**, as planned. M6 proves single-threaded replay
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
  serialised because write sets are table-granular. Recorded for M8, not redesigned.
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
| Strategy laboratory: synthetic cell field, map modes, pan and zoom, ID picking, mock data-oriented systems, speed controls including unbounded headless execution, replay, profiler counters | **Met**, as above. Controls are keyboard only; widgets wait for M9's command infrastructure, deliberately. |
| Documentation and ADRs that match the implementation | **Met** as of this commit; the E7 truth pass and this section are the evidence, and the next divergence will be found the same way. |

One requirement is met with a stated qualification (shaders bypass the registry) and none is
unmet. Not claimed, because not measured: bit-identical simulation across compilers beyond the
golden scenario on three platforms; graphics-processor time; and anything about
multi-threaded execution, which is M8.

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
decision, in [DEFERRED.md](DEFERRED.md). Top risks, as of 2026-09-15:

- **Nothing has ever been verified anywhere but this machine.** There is no git remote, so
  the three continuous-integration workflows have never run. Windows has never been compiled
  by anything. This is the largest risk in the project and it grows with every milestone.
- **The renderer is verified on one graphics processor and one software rasteriser.** The
  llvmpipe check covers Vulkan; Direct3D 12 remains unbuilt and unverified, and no real
  non-Apple hardware has ever run this code.
- Cross-architecture float divergence between arm64 and x86_64 (M6); mitigated by
  integer-first authoritative state and `-ffp-contract=off` from M0. The arm64-to-arm64
  comparison across two toolchains has been made and agrees; x86_64 remains unmeasured.
- No local Windows machine. The plan asked for a decision by M3 on acquiring hardware or a
  virtual machine, and M3 passed without the question being put. It is still open.
- Snapshot copy cost growth (M6–M8); mitigated by presentation-only snapshots and
  measurement before optimisation.
