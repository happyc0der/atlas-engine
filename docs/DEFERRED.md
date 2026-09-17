<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Deferred work

Everything consciously not built, and why. One list, because the reasons were previously
spread across three separate paragraphs of the roadmap, the charter and several decision
records, and a reason nobody can find is a reason nobody can challenge.

A deferral is not a to-do. Each entry says what would have to become true for the work to be
worth doing; several will never become true, and that is a fine outcome.

Last reviewed 2026-09-17, after M9.

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
  offscreen-then-blit and the identifier-target picking pass are those two. **M8 did not take
  it up, and that was right rather than an oversight:** M8 was profile-guided, and no profile
  pointed at pass ordering or target lifetime. The cost that did show up in the frame was
  instance data, which compaction fixed without touching pass structure. The bar is still met
  and the design must still be checked against both implementations; what is missing is a
  measurement or a third pass that makes hand-ordering the bottleneck.
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
- ~~**Command and undo infrastructure, and therefore scene editing.**~~ **Built in M9** as
  `atlas::edit`. The panel now takes an `edit::History` rather than a `const Scene&`, which
  keeps the compiler-enforced guarantee and adds editing behind it: the history exposes its
  scene as const and has no method yielding a mutable one.

### M6 — simulation kernel

- **An atomic shared pointer for the snapshot channel.** The natural primitive is unavailable:
  the development platform's standard library does not define `__cpp_lib_atomic_shared_ptr`,
  checked rather than assumed, and the deprecated free-function overloads are removed in C++26.
  A mutex guards the pointer, held for the length of a pointer copy once per tick and once per
  frame. Becomes an internal change if the support arrives.
- **Save migration code.** There is no second format version to migrate from, and migration for
  an imagined change would be untested code. The version check makes deferring it safe: a file
  this build cannot read is refused rather than misread.
- ~~**Worker-count invariance.** M8 by plan.~~ **Done in M8.** M6's contract needed no
  change, which was the point of deferring it: the compute phase moved onto workers as a
  scheduling change. Proved two ways — a test comparing an unsplit baseline against 1, 2, 4 and
  hardware concurrency minus one, and the acceptance path reporting one hash at every worker
  count. The partitioning rule that makes it hold is in docs/DETERMINISM.md.

### M9 — tooling and the scripting decision

- **Memory and allocation counters.** The blueprint lists them among the editor's panels. There
  is no custom allocator to instrument — the charter defers one until a recorded limitation
  justifies it — so a panel would report what the operating system reports, which is already
  available and not attributable to anything. Tracy in the `macos-profile` preset covers
  allocations when the question actually arises. Revisit if the allocator is ever built.
- **Record and play as buttons.** The controls panel shows whether a replay is being recorded
  and how many commands it holds, but cannot start or stop it: `record_applied_commands` is
  set when the kernel is built. A setter would let a recording begin mid-run, and a replay that
  does not start at tick zero cannot reproduce the run it came from. The status rows are the
  useful half; the buttons would be a trap.
- **A renderer resource and debug view.** The blueprint lists it among the editor's eight
  panels, and it is the one of the eight that M9 neither built nor recorded until this review
  caught the omission. The material exists: `rhi::Device::resource_counts()` reports buffers,
  textures, samplers and pipelines, and is currently called only by the device's own leak
  report and by tests. Five numbers do not need a panel; they need five rows, and adding them
  to the applications' statistics panels is the cheap version that would satisfy most of what
  the blueprint wanted. What a real debug view would add is per-resource detail — which
  texture, which pipeline, how large — and the registry to support that does not exist, because
  nothing has needed to ask. The condition is a resource leak or a memory figure nobody can
  account for, at which point per-resource detail is the thing that answers it and a count is
  not.

- **A simulation reset button.** F9 reloads from a file, which is the existing lifecycle path
  and is what the panel's Load does. Regenerating a world from its seed without a file is a new
  path, and nothing has asked for one.
- **Dock layout persistence and multiple viewports.** `IniFilename` is deliberately null, so
  the overlay writes nothing to disk and every run starts in a known state. Panels place
  themselves once and are then left alone. Multiple viewports are on the charter's deferred
  list, and the bar is a measured limitation rather than a preference.

- **Embedded scripting.** Decided in [ADR-0009](adr/0009-scripting-decision.md): deferred, with
  the boundary fixed now so that adopting it later changes no existing signature. Nothing
  outside the tick reaches simulation state except through a command, and nothing runs *during*
  a tick. The trigger is all three of: a recorded limitation a stamped command list cannot
  express; `CommandQueue`, `CommandHandler`, `SnapshotHeader` and the three format versions
  unchanged for two consecutive milestones; and the edit infrastructure existing, which it now
  does. WebAssembly is evaluated against Lua when that fires, on determinism grounds.

### M7 — Strategy Lab

- **Column-level read and write sets.** The schedule's granularity is the table, so the lab's
  two writers of `cells` (one touches `region_value`, the other `owner_index`) land in separate
  batches: three batches for four systems, observed and asserted in the lab's tests. Column
  sets would let them share one. Recorded from a single observation rather than acted on; M8's
  parallel scheduling was to be where it earned a decision. **M8 decided not to, on a
  measurement:** parallelising *within* a batch, across rows, gave 2.38x, and the three
  serialised batches were never the limit — the hash and the commit phase were. Column sets
  would let two systems share a batch that is already fast. The sharper condition is a workload
  where the batch boundaries rather than the rows dominate a profile, which one million cells
  over four systems does not produce.
- **The world hash's cost.** ~~Two ways out, both M8 work.~~ **Done in M8, and neither of those
  two was the answer.** Measurement killed both: hashing only the tables a tick wrote saves
  nothing, because the tables it writes are the whole cost, and parallel hashing would buy 13%
  of a tick for a worker pool. What worked was sequential and unglamorous — thirty-two bytes per
  step across four chains, with a final mix — taking the hash from 11.6 ms to 0.37 ms and a
  million-cell tick from 13.1 ms to 2.0 ms. `kHashAlgorithmVersion` is 2. The trap, which only
  measuring quality alongside speed caught, is that the unfinalised version is half as good a
  hash as the one it replaces; see `docs/PERFORMANCE.md`.

  **Parallel hashing, revisited later in M8: it is now the first candidate rather than a
  rejected one.** That rejection weighed 261 microseconds against a 2.0 millisecond tick. Since
  the compute phase became parallel the tick is 946 microseconds and the hash is 40% of it. The
  measurement was right about a tick that no longer exists, which is the ordinary condition of
  optimisation work rather than a mistake: what a candidate is worth depends on what else has
  been done.

  **Still not built at the end of M8, and the reason is not a performance one.** After the
  commit swap the tick is 694 microseconds and the hash is about 40% of it, which makes this
  the largest remaining item by some distance. Every way of hashing in parallel — per-table
  chunks combined in table order, or a tree over block hashes — produces a different value from
  the sequential hash of the same state. That is not a bug to be engineered away: a hash whose
  value depends on nothing but the bytes is exactly what makes the current one splittable
  *without* changing the answer, and a parallel scheme buys speed by giving that up. So it
  spends `kHashAlgorithmVersion` 3, invalidates every save and replay in existence, and
  re-records every golden value, a fortnight after version 2 did the same.

  **The condition is therefore an owner's decision rather than a measurement**, and it is the
  only item in this file shaped that way. What would make it worth taking: a tick where the
  hash dominates by more than it does now, or a batch of format-breaking changes worth paying
  the one migration for together. What would make it unnecessary: hashing less, rather than
  faster — the lab hashes every table every tick because its systems write every row every
  tick, and a workload with genuinely cold tables would make dirty-table hashing live again.
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
- **Snapshot pooling.** ~~Building the snapshot costs 1.44 ms at a million cells and allocates a
  fresh one each time. The pool is built when the counter says the allocation, not the fill, is
  what costs.~~ **Measured in M8, and the condition is not met, so it is not built.** Filling a
  reused snapshot takes 1.349 ms at a million cells; allocating a fresh one each time takes
  1.374 ms — under two per cent, and indistinguishable at a hundred thousand. The benchmark
  holds the previous snapshot while the next is built, as the channel does, so the allocator
  cannot simply hand back the block it just freed; the allocation is cheap anyway because it
  recycles. A pool would buy two per cent in exchange for knowing when the renderer has finished
  with a snapshot, which is lifetime complexity for nothing. `simulation/snapshot` against
  `simulation/snapshot_fresh` keeps the comparison honest if either ever changes.

  The cost is the fill, and the candidate worth recording is a different one: all four palette
  bands are filled every frame and the renderer reads one. Filling only the displayed band would
  cut it roughly fourfold, at the price of a refill when the mode changes — about 0.34 ms once,
  against 1 ms every frame. That trades away "a mode switch rebuilds nothing", which was a
  deliberate M7 property, so it is a decision rather than an optimisation and is not taken here.
- ~~**Widgets in the overlay.**~~ **Started in M9.** The sandbox's scene panel has the first
  two interactive widgets the overlay has ever had: a drag field for an entity's local
  position, and undo and redo buttons. Both go through `edit::History`, so the reasoning that
  deferred them is satisfied rather than waived. Still deferred, each for its own reason:

  - **A rename widget**, blocked on the platform. `platform::Event` has no text-input event,
    `platform.cpp` does not handle `SDL_EVENT_TEXT_INPUT`, and the overlay's key table covers
    only navigation keys, so a text field cannot receive a single character today. The
    `Rename` command exists and is tested; it is the widget that is missing. The next platform
    slice is a `TextInput` event plus character forwarding in `DebugUi`.
  - **Rotation, scale, sprite and camera widgets.** Their commands exist and are tested from
    the first slice. Recorded here so that "a command with no widget" reads as intended rather
    than as a gap.
  - **Reparent by drag.** The command exists; the gesture needs drag-and-drop targets on every
    tree node and visible feedback when a cycle is refused. A parent field in the inspector is
    the same command with a fraction of the surface, and comes first.
  - ~~**Widgets for the lab's simulation controls.**~~ **Built**, along with a log console and
    an asset status panel. Keys and buttons now produce the same request type and are applied
    by one function, so the two cannot drift.
- **The sandbox's zoom anchor on a high-density display.** `DemoScene` hands the pointer's
  logical coordinates to a camera whose viewport is in pixels, so a wheel zoom anchors at half
  the intended point on a two-times display. The lab converts; the sandbox should too. Found
  while making the lab's pick agree with its own analytic inverse, which is the kind of thing
  a cross-check exists to find.

## Deferred by the charter, until a recorded limitation justifies the work

A
custom allocator. A custom entity-component system. A work-stealing scheduler. Networking.
Physics. Animation. A plugin interface. Multi-viewport overlay windows. Filesystem watchers,
in place of which hot reload polls modification times. Audio, gamepad input, input method
editors, and localisation.

Each of these is a project-sized subsystem. The bar is a measured limitation in the thing
being built, not an expectation that one will appear.
