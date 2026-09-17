<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Deferred work

Everything consciously not built, and why. One list, because the reasons were previously
spread across three separate paragraphs of the roadmap, the charter and several decision
records, and a reason nobody can find is a reason nobody can challenge.

A deferral is not a to-do. Each entry says what would have to become true for the work to be
worth doing; several will never become true, and that is a fine outcome.

Last reviewed 2026-09-15, after M6.

## Open gaps in the infrastructure

None. The three found by the M0–M4 audit were closed on 2026-09-15:

- **The software-rasteriser check** exists as `tools/ci/linux_gpu.sh`, run locally by
  `tools/ci/docker_gpu.sh` and in continuous integration by `gpu-smoke.yml`. One script for
  both, so they cannot drift.
- **The Stop hook** refuses to finish on a tree that does not build and pass. It skips when
  nothing that could affect the build has changed, and never blocks twice in a row.
- **`tools/format.ps1`** matches the shell script's behaviour, directories, extensions and
  version pin.

The planned `shaders.yml` continuous-integration job was never written and is not a gap. Its
purpose, catching a shader edited without re-cooking, is served by the `shaders_current` test,
which runs everywhere the toolchain exists and skips itself where it does not. That is a
substitution, recorded here because it was never recorded anywhere else.

## Never verified, as distinct from deferred

Much of this section was closed on 2026-09-15, when a remote was created and continuous
integration ran for the first time. What it found is recorded in
[ROADMAP.md](ROADMAP.md#first-continuous-integration).

| What | State |
|---|---|
| Continuous integration | Runs, on every push. It found twelve distinct problems in its first seven attempts, across six corrective pushes. None could have been found locally: they are properties of the runner images, the checkout action, the triplets, and compilers this machine does not have. |
| macOS and Linux x86_64 | Both build and pass, in Debug and Release. Linux x86_64 is verified for the first time; the local container is arm64. |
| Windows | Compiles for the first time. It found two genuine portability bugs in code that had never been compiled by MSVC. |
| Cross-platform determinism | Measured, and since continuous integration began it is re-measured on every push: the golden scenarios run in `ctest` on all six jobs, so their exact hashes are checked on macOS arm64, Linux x86_64 and Windows x64 MSVC, plus Linux arm64 in the local container. MSVC is therefore no longer unmeasured — the row said so for longer than it was true. The scenarios are integer-only, so floating point in authoritative state remains an open question rather than an answered one. |
| A decision the plan asked for | The plan said the owner would decide by M3 whether to acquire Windows hardware or a virtual machine. M3 passed without the question being put. Continuous integration reduces the urgency; it does not answer the question, because the graphics path on Windows still has nothing verifying it. |

## Deferred with a reason, by milestone

### M2 — renderer

- **DXIL, and therefore Direct3D 12.** The compiler that produces DXIL has no macOS build.
  Windows would run the Vulkan backend. Becomes worth revisiting when a Windows machine joins
  the project. See [ADR-0006](adr/0006-shader-toolchain.md).
- **Graphics-processor timing.** The graphics library exposes no timestamp queries at all, so
  the timing that exists is processor-side around acquire, record and submit. Revisit if the
  library adds them.
- **Device-loss recovery.** Detection landed after the M0–M4 audit; recovery means recreating
  the device and every resource on it, which cannot be tested on the hardware this project has.
  Out of scope for v0.1 by the charter.
- **Precompiled Metal libraries.** Needs the full development environment rather than the
  command-line tools. The shading language is supplied as source at run time instead.

### M3 — camera and batching

- **A render graph.** ADR-0002 set the bar at two real multi-pass features, so that it is
  designed against something rather than imagined. **Status changed in M7:** capture's
  offscreen-then-blit and the identifier-target picking pass are those two. The question is
  live for M8, and the design must be checked against both implementations, in particular
  against what each does with target lifetime and pass ordering, before a line is written.
- **Rotated sprites.** The batcher draws axis-aligned rectangles. A scene can express a
  rotation the renderer cannot draw. First deferred in M3 with "picked up when the renderer
  next changes, in M7". The renderer changed in M7 and this was deferred again, on a stated
  reason rather than by lapse: the Strategy Lab is axis-aligned, and at a million cells every
  added instance byte costs a megabyte per upload in the exact path M7 made faster. The silent
  drop in the sandbox scene demo becomes a logged warning, so a scene expressing something the
  renderer cannot draw is visible. Picked up by the first consumer that needs a rotated
  instance, or when instance data is next redesigned.

### M4 — assets

- **An asset status panel.** The overlay's only consumer reports asset counts in its exit
  summary, and a panel would be a second place for the same three numbers to be wrong.
- **Shader loading through the registry.** Shaders still come from the generated manifest. The
  registry has nothing to add to a shader whose resource counts are compile-time constants.
- **Cooked-artifact caching.** ~~Nothing yet takes long enough to import to justify a cache.~~
  **Built in M7 Gate 1**, because the charter lists it under v0.1. The M4 reasoning turned out
  to be half right: the first design, keyed on a content hash, cost more to key than to decode
  (15.3 ms against 8.25 ms at 2048²) and was reported rather than kept. Re-keyed on the owner's
  decision to size plus modification time plus importer version, a warm import at 2048² is
  2.26 ms against 8.46 ms cold. Both designs and both sets of numbers are in
  `docs/PERFORMANCE.md`.
- **An asset dependency graph.** With one asset type, nothing depends on anything.

### M5 — scene

- **The `runtime` module.** Planned here on the assumption a second application would need the
  same composition. In M7 the second application arrived and the assumption was tested line by
  line against `apps/sandbox/main.cpp`: what would be duplicated split into about 195 lines of
  pure, untested utilities that were identical, and a frame loop that was not, because the
  sandbox loop has no kernel and no snapshot publication while the lab's has no scene graph.
  The utilities became `apps/common`, a static library with the tests they never had. A module
  for the loop would have been a second `main` depending on every other module, and would have
  made the dependency table less informative than it is. Re-deferred with a sharper condition:
  a third application, or the two loops converging in shape rather than merely in ingredients.
- **Command and undo infrastructure, and therefore scene editing.** The scene panel takes the
  scene by const reference, so the compiler enforces read-only rather than discipline. Mutation
  arrives in M9 with the infrastructure that routes every change through one validated path.

### M6 — simulation kernel

- **An atomic shared pointer for the snapshot channel.** The natural primitive is unavailable:
  the development platform's standard library does not define `__cpp_lib_atomic_shared_ptr`,
  checked rather than assumed, and the deprecated free-function overloads are removed in C++26.
  A mutex guards the pointer, held for the length of a pointer copy once per tick and once per
  frame. Becomes an internal change if the support arrives.
- **Save migration code.** There is no second format version to migrate from, and migration for
  an imagined change would be untested code. The version check makes deferring it safe: a file
  this build cannot read is refused rather than misread.
- **Worker-count invariance.** M8 by plan. M6 proves single-threaded replay determinism and
  ships the contract M8 needs; doing both at once would double the debugging surface.

### M7 — Strategy Lab

- **Column-level read and write sets.** The schedule's granularity is the table, so the lab's
  two writers of `cells` (one touches `region_value`, the other `owner_index`) land in separate
  batches: three batches for four systems, observed and asserted in the lab's tests. Column
  sets would let them share one. Recorded from a single observation rather than acted on; M8's
  parallel scheduling is where it earns a decision.
- **The world hash's cost.** ~~Two ways out, both M8 work.~~ **Done in M8, and neither of those
  two was the answer.** Measurement killed both: hashing only the tables a tick wrote saves
  nothing, because the tables it writes are the whole cost, and parallel hashing would buy 13%
  of a tick for a worker pool. What worked was sequential and unglamorous — thirty-two bytes per
  step across four chains, with a final mix — taking the hash from 11.6 ms to 0.37 ms and a
  million-cell tick from 13.1 ms to 2.0 ms. `kHashAlgorithmVersion` is 2. The trap, which only
  measuring quality alongside speed caught, is that the unfinalised version is half as good a
  hash as the one it replaces; see `docs/PERFORMANCE.md`.
- **A single-threaded tick at a million cells.** ~~33 ms, so 60 ticks a second is out of reach
  at that size until M8's parallel simulation.~~ **2.0 ms since the hash change**, and the lab
  holds 60 ticks a second at a million cells with a 10.7 ms median frame. Parallel simulation is
  no longer what stands between the lab and its target, which is worth knowing before M8 spends
  effort on it. The lab's catch-up limit went back to the scheduler's default of 8 for the same
  reason: the low limit was a symptom of the slow tick and, measured, now costs dropped ticks.
- **Instance compaction for the cell field.** ~~Each drawn cell is a 48-byte quad instance
  rebuilt from the snapshot every frame.~~ **Done in M8.** Four bytes a cell, the rectangle
  derived in the shader from the instance index and a per-run uniform, which is the identifier
  pass's technique applied to the picture. Submitting a million cells went from 7.11 ms to
  303 us, the instance buffer from 48 MB to 4 MB, and the resident per-cell geometry to nothing.
  Two thirds of that gain came not from the compaction but from what the compaction revealed:
  a `push_back` per cell that had been hidden behind the copies it was feeding. The picture is
  unchanged to within one least significant bit, checked byte by byte. See
  `docs/PERFORMANCE.md`.
- **Snapshot pooling.** Building the snapshot costs 1.44 ms at a million cells, once per frame
  after the last tick, and allocates a fresh one each time. The counter is in the overlay; the
  pool is built when the counter says the allocation, not the fill, is what costs.
- **Widgets in the overlay.** The lab's controls are keyboard only. That satisfies M7 and adds
  no engine surface for one caller; widgets arrive with M9's command and undo work, so that no
  widget becomes a second unvalidated path into state, the same reasoning that kept M5's scene
  panel read-only.
- **The sandbox's zoom anchor on a high-density display.** `DemoScene` hands the pointer's
  logical coordinates to a camera whose viewport is in pixels, so a wheel zoom anchors at half
  the intended point on a two-times display. The lab converts; the sandbox should too. Found
  while making the lab's pick agree with its own analytic inverse, which is the kind of thing
  a cross-check exists to find.

## Deferred by the charter, until a recorded limitation justifies the work

The `tasks` module and its worker pool, created with the first parallel benchmark in M8. A
custom allocator. A custom entity-component system. A work-stealing scheduler. Networking.
Physics. Animation. A plugin interface. Multi-viewport overlay windows. Filesystem watchers,
in place of which hot reload polls modification times. Audio, gamepad input, input method
editors, and localisation.

Each of these is a project-sized subsystem. The bar is a measured limitation in the thing
being built, not an expectation that one will appear.
