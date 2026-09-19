<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Deferred work

Everything consciously not built, and why. One list, because the reasons were previously
spread across three separate paragraphs of the roadmap, the charter and several decision
records, and a reason nobody can find is a reason nobody can challenge.

A deferral is not a to-do. Each entry says what would have to become true for the work to be
worth doing; several will never become true, and that is a fine outcome.

Last reviewed 2026-09-19, while planning M16.

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
- ~~**Rotated sprites.**~~ **Built in M13**, by the trigger this entry named: the first consumer that needed a rotated instance was an animation clip with a rotation track. The instance grew from 48 bytes to 64 and the submit path costs a fifth to a third more, measured against a prediction written first; see PERFORMANCE.md. Note that the re-deferral's own reason had expired before it was fired — it cited the cost per instance byte at a million cells, and since M8 the cell field has its own four-byte stream and does not use quad instances at all. The original text follows. The batcher draws axis-aligned rectangles. A scene can express a
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
- **An asset dependency graph.** ~~With one asset type, nothing depends on anything.~~ **That
  reason expired in M12**, when there were four. The conclusion still holds and the reason is
  now different: no asset the engine loads *names* another. What M13 added is the first pair
  that depends on the other in **meaning** without saying so — an animation clip cuts a sprite
  sheet into a grid, and names neither the sheet nor its size, so a clip written for a
  four-by-two sheet shows the wrong frames on a three-by-three one and nothing reports it.
  Import refuses a cell outside the clip's own grid, which catches a clip disagreeing with
  itself and cannot catch a clip disagreeing with a texture it never sees. **Picked up when
  something is actually mis-framed by it**, or when a second such pair appears; the fix is a
  clip declaring what it needs and the registry resolving it, which is a change to how every
  asset is requested rather than to clips.

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
- **Save migration code.** Still deferred, and narrower than it was. **The scene format reached
  version 2 in M13 and needed no migration code**, which is not a postponement but a rule:
  [ADR-0012](adr/0012-scene-format-v2.md) says a version that only *appends* components is read
  by accepting a range of versions and branching on none of them, and the test states the
  property literally — a version 1 document loads, re-saves, and equals itself with only the
  version number changed. What remains deferred is migration for a change that is **not** a pure
  append: a renamed key, a changed meaning, a removed component. There is no such change to
  migrate from, and code for an imagined one would be untested code. The simulation's save format
  still has exactly one version. The version check makes deferring it safe either way: a file
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

  - ~~**A rename widget**, blocked on the platform.~~ **Built in M11**, which gave the platform
    a `TextInput` event, completed the overlay's key table, and started submitting modifiers —
    without which no shortcut inside a text field could fire. The log console's category filter,
    which had been a real field unable to receive a character since M9, started working in the
    same commit.
  - **Rotation, scale, sprite and camera widgets.** Their commands exist and are tested from
    the first slice. Recorded here so that "a command with no widget" reads as intended rather
    than as a gap.
  - **Reparent by drag.** The command exists; the gesture needs drag-and-drop targets on every
    tree node and visible feedback when a cycle is refused. A parent field in the inspector is
    the same command with a fraction of the surface, and comes first.
  - **Composition preview in the overlay.** M11 delivers `TextEditing` from the platform, and
    the overlay deliberately does not draw it: the library has no composition interface, and
    its own window-system backend ignores the event too. So a preedit appears where the
    operating system draws it, and is invisible until committed where the operating system
    does not. Picked up if the overlay ever gains a text field that a person composes into
    rather than types a short identifier into.
  - **The candidate-list events.** `SDL_EVENT_TEXT_EDITING_CANDIDATES` is sent only to an
    application that has taken over drawing the candidate list, which Atlas has not.
  - **An operating-system clipboard for the overlay.** The library's default clipboard is
    in-process, so pasting from another application into a field does nothing. `Window` would
    need to expose the system clipboard. Picked up the first time someone tries to paste a
    path into the overlay and is surprised.
  - ~~**Widgets for the lab's simulation controls.**~~ **Built**, along with a log console and
    an asset status panel. Keys and buttons now produce the same request type and are applied
    by one function, so the two cannot drift.
- ~~**The sandbox's zoom anchor on a high-density display.**~~ **Fixed in M9**, in four sites
  rather than the one this entry described: both of the sandbox's scene classes had it, for pan
  as well as zoom. The entry was left standing by mistake and is struck here; M11's shared
  camera controller is what stops the next such fix needing four edits.

- **Gamepad features beyond pan, zoom and the lab's controls.** M11 built the slots, the
  buttons, the axes and the dead zone against the two consumers that exist. What it did not
  build, each for the same reason — nothing asks for it:

  - **Gamepad navigation of the overlay.** The library has a navigation mode that wants a pad
    wired into it. A person using a pad today can drive the camera and the lab's controls but
    cannot reach a panel. Picked up when a build is meant to be usable without a keyboard,
    which no application here is.
  - **Radial or configurable dead zones.** The dead zone is per-axis with fixed constants,
    which is the right default and is wrong for a stick pushed diagonally: each axis clears
    its threshold separately, so the corners are slightly favoured. A radial zone treats the
    stick as one vector. Picked up when someone with a controller in hand says the diagonals
    feel wrong, which is the only way that judgement can honestly be made, and it needs the
    constants to become configuration at the same time.
  - **More than four gamepads.** Four is what local multiplayer and every console convention
    assume. A fifth pad is refused with a warning rather than silently dropped, so the limit
    is visible when it is reached.
  - **Rumble, sensors, touchpads, paddles and battery state.** The window system exposes all
    of them. None has a consumer, and each would be another thing to keep working across three
    platforms for no user.

### M12 — audio

- **A callback mixer on the window system's audio thread.** [ADR-0011](adr/0011-audio.md)
  chose main-thread push mixing, and this is its recorded rollback rather than a gap: the mixer
  is already pure functions over buffers, so what changes is who calls them. It buys immunity
  to a long frame and latency of roughly 20 ms against the present 70 to 80. It costs a thread
  on which nothing may allocate, log or assert — a rule that is invisible until violated, and
  most easily violated by someone adding a log line to diagnose something else. **Picked up on
  a latency complaint above about 80 ms**, or the first consumer that needs feedback tied
  tightly to an input, which a strategy game does not have.
- **Ogg Vorbis, streaming, and a music slot.** The decoder is installed on every platform
  already, at no cost, and M12 still declined it by owner decision: it forces the repository's
  first committed binary test fixture, because an Ogg cannot be produced by a checked-in
  standard-library script the way a WAV can. **Picked up when a track is long enough that 64 MB
  decoded against 4 MB encoded matters**, which needs a game. Nothing built in M12 is discarded
  when that happens: a stream is a second asset type beside the clip, not a replacement for it.
- **A decoded-audio cache.** The artifact cache is texture-shaped end to end: its entry header
  is a width, a height and a byte count, its files end in `.texture`, and it carries a single
  importer-version constant. Widening all of that to avoid a copy nobody has measured is the
  mistake M4 made once with textures and reported rather than kept. **Picked up when an audio
  import is measured above 5 ms**; the committed loop imports in well under one.
- **A better resampler.** Linear interpolation, which is exact when the rates match and adds
  audible artefacts to content with strong high frequencies. Inaudible on clicks and beds at a
  two-to-one ratio. **Picked up by the first consumer that can hear it**, which means real
  recorded music rather than generated tones.
- **Per-voice pitch, spatial audio, and any effect at all.** No consumer. Pitch is the cheapest
  and would arrive first, as a step through the clip other than one frame at a time.
- **More than three buses, or a graph instead of scalars.** The two consumers want "quieter
  overall", "quieter music" and "quieter effects", which is three multiplications. A graph is
  what a mixer becomes when something needs one.
- **An audible fallback for a missing sound.** Deliberately not built, and not only unbuilt: a
  missing texture resolves to a magenta checkerboard because something must still be drawn, and
  a missing sound has nothing it must still do. Inventing a noise would be worse than silence.
- **A limiter.** The mix clamps, which distorts rather than ducking when many loud voices
  coincide. Thirty-two voices would have to be loud at once for it to matter.
- **Device change events.** Unplugging headphones mid-run is not handled; the window system
  migrates the default device underneath the stream, which covers the common case and not the
  case where the device disappears entirely.
- **MP3, FLAC and Opus.** No consumer, and each is another decoder of untrusted input to
  harden.

### M14 — networking

- **A command source that can only mark its own turns.** M14 built one — a handle binding the
  turn gate to a single identifier, so that "mark only your own turn" was unrepresentable rather
  than documented — and then removed it in the same milestone, because the first real
  implementation wanted the opposite. A lockstep session speaks for every peer it is connected
  to and must mark all of their turns; the only producer that should be held to one identifier is
  an untrusted one, and there is not one yet. The constraint belongs at the boundary with the
  untrusted thing rather than in the interface every producer shares. **Picked up by M15's mod
  host**, which is that boundary: it can hand a mod a restricted handle without the interface
  itself being restricted. Recorded rather than left implicit because building it early was a
  mistake worth being able to find again.
- **A transport.** No socket, by decision (ADR-0014). The criteria are recorded there rather than
  resolved: a reliable ordered channel, because lockstep tolerates no loss and raw datagrams
  would mean writing reliability; Windows support in the pinned baseline; licence compatibility;
  whether encryption and traversal are wanted; and whether it may own a thread. Picked up when
  two machines need to play, which no consumer needs today.
- **Resynchronisation after a divergence.** Detected, attributed and stopped, the same treatment
  device loss gets. Recovering would mean shipping state, which is the thing lockstep exists not
  to do. Picked up if a consumer would rather continue wrongly than stop.
- **More than sixteen peers, spectators, and a relay topology.** A relay costs nothing later
  because a turn is addressed by source rather than by socket. Picked up by a consumer.

### M13 — animation

- **A generalised asset payload, and a registration point for importers.** The registry keeps
  one optional payload per type side by side in every entry and dispatches by a switch. That is
  four payloads across five types after this milestone, and every edit needed to add another is
  one the compiler points at, because the switch is exhaustive and a missing arm is an error.
  **This entry exists because there was none**: the arrangement has been questioned twice, at
  three types and at four, and kept both times without the reasoning being written anywhere.
  Picked up when a payload is large enough that carrying it in every entry costs something
  measurable, or when something outside this repository needs to add a type of its own — at
  which point the switch stops being a complete list and becomes a limitation.
- **An importer cache for clips.** A clip is a few hundred bytes of text that parses faster than
  a cache entry would read, and the artifact cache is texture-shaped end to end. Same trigger as
  audio: an import measured above five milliseconds.
- **Snapshot interpolation into the pose.** The architecture's own time table has said since M6
  that render time carries an `alpha` in [0, 1) for "interpolation and visual effects", and
  `alpha` has never interpolated anything: its only readers pace a headless frame. The pose is
  the obvious place to put it — an entity whose position comes from a simulation snapshot would
  be drawn between the last two rather than snapping to the newer. **It needs more than the
  pose**: the snapshot channel is latest-wins and keeps one, so there is nothing to interpolate
  *between* until it keeps two, and that is a change to the channel rather than to animation.
  Picked up by the first entity whose position comes from a snapshot, which no application has
  today because the lab's cells are snapshot rows and not scene entities.
- **Packed atlases.** Frames address cells of a uniform grid, which is what a person can write by
  hand and what survives a sheet being re-exported at another size. A packed atlas needs a
  generated table naming each frame's rectangle, and therefore a tool between editing a clip and
  seeing it. Picked up by the first sheet whose frames are not the same size, which is usually
  the first sheet drawn by an artist rather than generated by a script.
- **Reverse playback, and a negative speed.** `Animator::speed` is clamped to [0, 100] and the
  clock is unsigned, deliberately: letting time run backwards would make the integer clock's
  exactness pointless and would need every sampler to decide what "the key in force" means when
  time decreases. Running a clip backwards is a different feature with a different name. Picked
  up by a consumer that needs one, which a map game plausibly never has.
- **Blending between clips, and frame events.** One clip per entity, and a clip that produces
  values rather than one that fires callbacks. Blending needs a second clock and a weight per
  entity and a rule for what to do when the two clips disagree about a frame; events need a
  delivery order and a decision about what a callback may touch, which for something that runs
  outside the tick is most of a design. Neither has a consumer. Picked up when a transition looks
  wrong without a blend, or when something needs to happen *at* a time in a clip rather than
  *because of* one.
- **Skeletal animation.** No consumer, and none in prospect: a map-based strategy game draws
  sprites and regions, not jointed figures. Recorded so that the absence is a decision rather
  than an oversight. Picked up if the game ever draws something with limbs.
- **Choosing a clip from the inspector.** The animator panel can start, stop, scrub, re-speed,
  change the loop mode and remove an animator, and it cannot say *which clip*. An animator names
  an asset by an identifier that is a hash of a path and a type, and there is no way back from
  one to the other: the panel would have to offer the paths that exist, which means browsing a
  mounted filesystem, which is a file picker rather than a widget. Text input was the blocker
  until M11 and is no longer; the remaining work is real and is about assets, not about
  animation. The panel shows the identifier so that a wrong clip is at least visible. Picked up
  when a scene is authored in the editor rather than built in code, which is the first time
  somebody needs to attach a clip they did not compile in.

### M15 — sandboxed mods

- **A thread for mods.** Everything runs on the main thread with the kernel. A mod costs about
  0.2 µs a tick, so eight of them are 0.01% of a frame; the trigger is a mod expensive enough to
  measure, and the cost of moving is that a mod would then need its own copy of the views, since
  a span into the world is only valid between ticks.
- **Hot reload of mods.** A mod is loaded once at startup. Reloading one mid-session would change
  what every peer must agree on, so it needs a session-wide agreement first — which means it is
  really a networking feature. Trigger: a mod author asking, and a design for announcing it.
- **A mod manager panel.** The lab takes one mod from `--mod` and reports what it did in a log
  line. A panel needs somewhere to put it and more than one mod to list.
- **Lua compiled to WebAssembly as an authoring route.** ADR-0015's answer to the authoring cost
  it accepted: a Lua interpreter compiled to wasm32 runs inside this sandbox, where even its
  address-dependent behaviour is deterministic because linear memory is. A committed artefact
  like a cooked shader. Trigger: a mod author who will not write AssemblyScript or C.
- **Mod signing, and per-mod persistent storage.** Neither has a consumer. Storage in particular
  would be state outside the command queue, so it needs its own record before it needs code.
- **Typed snapshot views in the engine.** Views are bytes on purpose: the engine has no game
  state to describe (ADR-0010 D7). The game's repository types them.
- **More than one mod in the lab.** `ModHost` is one mod each and the identifier is a
  constructor argument, so the mechanism is there; the lab takes one because one is what the
  proofs need. Trigger: anything that needs two mods to interact.
- **A large-module load measurement.** `script/load` is 5 µs for a 295-byte module, which says
  nothing about a real one. Trigger: the first mod big enough to notice.

### M16 — localisation

Recorded on 2026-09-19 while M16 was being planned, before any of it was built, because it is a
fact about the tree today rather than a consequence of the milestone.

- **Glyphs outside Latin-1, and the font that would carry them.** The overlay renders Dear
  ImGui's default baked atlas: Basic Latin and Latin-1 Supplement, and nothing else. Atlas never
  touches `io.Fonts` — there is no `AddFont*` call, no `ImFontAtlas`, no glyph range anywhere in
  the tree — and `vcpkg.json` pins `imgui` with `docking-experimental`, `sdl3-binding` and
  `sdlgpu3-binding` but **no `freetype` feature**, so even ImGui's own path is the stb_truetype
  fallback.

  The consequence is worth stating plainly, because a string table invites the opposite
  conclusion: **a second language is a data change only if it is Western European.** French,
  German, Spanish and Italian would render from a table alone. Polish, Czech, Turkish, Greek,
  Russian, Hebrew, Arabic and every CJK language would show blanks or boxes, because the glyphs
  are not in the atlas. For those, a string table is necessary and not sufficient.

  Not built: configuring the atlas from the loaded table's locale, and bundling a font with
  wider coverage. Trigger: the first language outside Latin-1. The cost is a font file, its
  licence and provenance, its size — a CJK face is megabytes against the current atlas's
  kilobytes — and a decision about whether the atlas is rebuilt when the locale changes or
  fixed at startup.

- **Anything that measures or folds text.** One place in the repository interprets UTF-8 at all:
  `engine/platform/src/text_split.hpp`, which backs a cut off a continuation byte so M11's text
  events split on character boundaries. Everywhere else a string is opaque bytes, and the two
  case-folding sites — `assets/src/virtual_path.cpp:135` and `rhi/src/device_loss.cpp:30` — are
  byte-wise ASCII and would be wrong for anything else. Nothing measures display width, walks
  grapheme clusters, or normalises. Trigger: the same one, and it arrives first.

## Decided by ADR-0010, planned as milestones

Networking. Sandboxed mods. Animation. Audio. Gamepad input. Input method editors.
Localisation.

These seven left the recorded-limitation regime on 2026-09-17 by owner decision, recorded in
[ADR-0010](adr/0010-charter-amendment.md). The condition for building each is now that its
milestone is scheduled and has a consumer to build against; the condition for dropping each is
that no consumer strong enough exists when its turn comes.

## Deferred by the charter, until a recorded limitation justifies the work

A custom allocator. A custom entity-component system. A work-stealing scheduler. Physics.
Multi-viewport overlay windows. Filesystem watchers, in place of which hot reload polls
modification times.

Physics has a trigger written down: a consumer that needs bodies interacting through forces
rather than through commands. A map-based strategy game has none, and what resembles physics in
one is either picking, which exists, or a presentation tween, which the animation milestone
provides.

Each of these is a project-sized subsystem. The bar is a measured limitation in the thing
being built, not an expectation that one will appear.
