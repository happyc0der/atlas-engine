# Atlas — working rules

Atlas is a C++23 **engine platform** for a future map-based grand-strategy game.
Build the engine. Never implement the game: no countries, wars, diplomacy, economies,
historical data, political borders, or game-specific scripting. Synthetic test data may
use neutral names (RegionValue, OwnerIndex, ColorIndex) and must carry no game meaning.

## Commands

Replace `macos-debug` with `linux-clang-debug` or `windows-msvc-debug` as appropriate.

```sh
cmake --preset macos-debug            # configure
cmake --build --preset macos-debug    # build
ctest --preset macos-debug            # test (excludes label "gpu")
ctest --preset macos-debug -L integration  # only the end-to-end sandbox checks
ctest --preset macos-debug-gpu        # test including GPU tests (M2+, real GPU only)
cmake --workflow --preset ci-macos-debug   # configure + build + test

tools/format.sh --check               # clang-format, --fix to apply
pwsh tools/format.ps1 -Check          # the same, on Windows
tools/tidy.sh macos-debug             # clang-tidy over first-party targets
python3 tools/check_module_deps.py    # module boundary + cycle + exception check
python3 tools/check_spdx.py           # licence headers
tools/precheck.sh macos-debug         # everything above, in order
tools/ci/docker_linux.sh              # Linux build in a container, before pushing
tools/ci/docker_gpu.sh                # GPU tests on a software rasteriser, in a container
```

Profiling build: preset `macos-profile` (Tracy on). A default build contains no Tracy
symbol at all.

Running the sandbox:

```sh
./build/macos-debug/bin/atlas_sandbox                             # a window; Escape quits
./build/macos-debug/bin/atlas_sandbox --headless --ticks 600      # no window at all
./build/macos-debug/bin/atlas_sandbox --video-driver dummy --frames 30   # window, no display
./build/macos-debug/bin/atlas_sandbox --headless --unbounded --ticks 1000000
./build/macos-debug/bin/atlas_sandbox --frames 20 --screenshot /tmp/frame.ppm
./build/macos-debug/bin/atlas_sandbox --hot-reload      # re-read changed asset files
```

Sanitizers: presets `macos-asan`, `macos-tsan`, `linux-clang-asan`, `linux-clang-tsan`.
Never combine ASan and TSan. TSan runs only `unit` and `determinism` labelled tests.

## Architecture invariants

- Module dependencies are declared in `cmake/ModuleGraph.cmake` and enforced at configure
  time. Adding an edge means editing that table, with a reason.
- Third-party libraries are `PRIVATE` links. They must not appear in any public header
  except where an ADR allows it (Tracy in `core/profile.hpp`, EnTT in `scene`).
- SDL types live only in `engine/platform/src`, `engine/rhi/src`, `engine/tools/src`,
  `engine/audio/src`, and the `atlas::platform_internal` target. Never in `include/`.
- `renderer` reads immutable snapshots; it never mutates simulation or scene state.
- `scene` is presentation only. It is not the grand-strategy database.
- Entities are referred to by `StableId`. The entity library's handle is never stored,
  serialized, or exposed: it is recycled and means nothing outside one run.
- Anything observable from `scene` is ordered by stable identifier, not by the entity
  library's storage order: iteration, draw order, sibling lists, and the saved file.
- Saved files name and version themselves, refuse a version they do not know, and are
  treated as hostile input. A failed load changes nothing.
- The scene is edited only through `edit::History`, which applies undoable commands and
  exposes its scene as const. Panels take the history, never a mutable `Scene&`, so the
  compiler still enforces that no widget bypasses validation.
- **Authored components belong to the history; derived ones belong to whoever computes them,
  and neither writes the other's fields** (ADR-0012). The animator writes only
  `AnimationPose`, which is composed on top of the authored transform and never serialised, so
  an entity can be edited while it plays. This replaced an earlier rule forbidding an
  application to animate what the user can edit; that rule made the editor unusable on the one
  scene it existed to edit.
- A component added to the scene is added in five places or it is added wrongly: the component,
  the `Scene` accessors, `EntityView`'s flags, the serialiser's fixed key order — appended,
  never inserted — and `edit::Destroy`'s record with both its loops. Miss the last and
  destroying the entity then undoing loses the component in silence.
- Recompose world transforms every frame, unconditionally. The history deliberately does not:
  it bumps a revision and leaves that to whoever is watching. Something that recomposes only
  sometimes recomposes wrongly, which is how an edit came to change a number and not the
  picture.
- `simulation` contains tick scheduling, commands, hashing, and system contracts.
  It contains no game rules.
- A tick is always: drain and apply commands, compute, commit, hash. Nothing reaches
  simulation state except through a command.
- Compute takes a `const World` and commit a mutable one. A system writes only storage it
  owns during compute. Never widen that.
- Authoritative state is integer by default. Floating point in it needs a recorded reason
  and a passing golden-hash comparison (ADR-0008).
- Random numbers come from a named stream keyed by seed, stream, tick and counter. No
  ambient generator, no state carried between ticks.
- The golden-scenario hashes are written down. If a change makes that test fail, decide
  whether the change was meant to alter simulation results, and say so.
- Applications are composition roots. Reusable logic belongs in a module.
- Cyclic module dependencies are forbidden.

## Coding rules

- RAII for every owned resource. Values and `unique_ptr` by default; `shared_ptr` only
  for immutable snapshots.
- Errors: `atlas::Result<T>` = `std::expected<T, atlas::Error>`. No exceptions across
  module or thread boundaries. `try`/`catch` only in commented boundary wrappers.
- Generation-counted `Handle<Tag>` for long-lived engine resources, never raw pointers
  that can go stale.
- No hidden global mutable state, no service locators. The log sink registry is the one
  permitted process-wide object.
- Explicit-width integers in serialized state. Stable IDs, never serialized pointers or
  container positions.
- `std::span` for non-owning ranges. No accidental bulk copies.
- No `std::print` in `engine/` (libstdc++ 13 on the Ubuntu CI image lacks `<print>`).
  Use `std::format` plus the logger.
- Never iterate `std::unordered_*` where the order is observable in output, a hash, or a
  reduction. Lookups are fine.
- Destructors must not log: formatting allocates, and a throwing destructor ends the
  process. Log around the lifetime instead.
- Platform and window calls are main-thread only. Assert it with `ATLAS_ASSERT_MAIN_THREAD`
  at every entry point.
- Query window state such as minimised or focused; do not track it from events, which
  drifts when an event is missed.
- `platform::Event` is trivially copyable, and a `static_assert` says so. A payload that would
  allocate inside `pump()` or dangle after the next one does not belong in an event: copy it
  inline, and split it across several events if it does not fit.
- Text input is switched on only while something is focused, and the window is asked whether
  it is on rather than told and remembered. Always-on routes every key through an input method
  and makes every shortcut key type as well.
- A `GamepadId` is a slot, not a device. The window system's own device identifier never
  leaves `engine/platform/src`: it is not stable across runs and would end up in a saved
  binding. Face buttons are named by position, never by letter.
- A dead zone is applied once, in the platform, with a rescale so full deflection still reads
  exactly one. Applied a second time by a consumer it is a bug.
- A key and the gamepad button that mean the same action live on the same row of one table, so
  they cannot drift apart.
- SDL types stay in the module implementations named above. The native window handle reaches
  the renderer only through `atlas::platform_internal`.
- **The platform brings SDL subsystems up; nothing else does.** Its destructor shuts all of
  them down at once, so a second module starting one would share a lifetime it cannot see.
  A module that needs a device opens one on a subsystem the platform started (ADR-0011).
- Audio is presentation. A sound is triggered where a command is submitted or where a snapshot
  is observed, never inside a system or an `apply`. Nothing audio produces is ever hashed.
- Animation is presentation too, and is driven by frame time rather than by ticks: what a clip
  shows must not depend on how many simulation steps a frame happened to run. Nothing it
  produces is hashed or reaches a saved file. It is not deterministic across machines, because
  frame time is not; it is bit-exact given the same sequence of steps on one build, and that is
  what every test of it rests on.
- A clock is integer. Milliseconds in a file, nanoseconds in memory, and no float accumulation
  anywhere between them. A float sum has drifted after an hour and an integer one has not, which
  is the whole reason "wrap or accumulate" is not a question anybody has to answer here.
- An easing is exact at both endpoints, and a blend is written `(1 - t) * a + t * b` for the
  same reason: a key's own value is then comparable with `==` rather than with a tolerance.
- A step taken from the frame is clamped before it reaches a clock. A debugger stopped at a
  breakpoint produces a frame a second long, and the frame after it must not jump every clip
  forward by a second.
- Mixing happens on the main thread, once a frame (ADR-0011). Registering a callback with the
  window system's audio thread needs a new ADR, because it adds a thread on which nothing may
  allocate, log, or assert.
- A decoder chooses its format from the leading bytes, never from a path's extension. The
  extension is a claim made by whoever named the file, and an importer's input is untrusted.
- An asset type added without a finaliser sits in `Decoded` for ever. The registry counts and
  reports that, because it cannot know which types have one; add the finaliser in the same
  change as the type.
- Every committed binary asset is either generated by a checked-in script with a `--check`
  lint test, or listed in `assets/source/PROVENANCE.md` with its origin and licence.
- **A `--check` that compares bytes must compare bytes only this repository decides.** A
  generated file whose content passes through a compressor — zlib inside a PNG, a shader
  compiler inside a SPIR-V module — is comparing library versions rather than content, and such
  a check passes by coincidence until two machines disagree. Write the bytes uncompressed, or
  compare something other than bytes, and say at the site which was chosen. M13 found this
  twice: in the shader currency check, which had been passing by luck since M2, and again before
  a sprite sheet was committed.
- Shaders are authored in HLSL and cooked to SPIR-V and MSL. Commit the cooked outputs.
- Shader resource counts come from reflection, never from hand-written numbers.
- A matrix goes to a uniform through `uniform_elements()`, not `elements()`: shaders read
  column-major and Atlas stores row-major.
- Benchmarks must exclude presentation, or they measure the display rather than the engine.
- Asset paths are validated by `VirtualPath`, never assembled by hand. Upward traversal,
  absolute paths, backslashes and null bytes are refused, and a resolved path is checked to
  lie inside its mounted root.
- Asset workers touch bytes only: never the window, the device, or engine state. Creating a
  graphics resource is main-thread work, which is why `Decoded` and `Ready` are different
  states.
- A missing or broken asset must resolve to a fallback and be recorded, never stop the
  engine.
- Device loss is detected and reported, never recovered from. Once lost, every `rhi` entry
  point fails immediately with `DeviceLost` rather than attempting work and failing
  differently. Detection is reliable on Vulkan, best effort on Direct3D 12, and unavailable
  on Metal.
- Behaviour that only exists when the whole program runs — construction order, shutdown
  order, exit codes, lifecycle logging — belongs in `tests/integration`, because no unit
  test can reach it.
- No per-frame allocation in measured hot loops after warm-up.
- Assertions are for violated programmer invariants. Recoverable user or data errors
  return an `Error` with context.
- Every public API documents ownership, lifetime, thread affinity, and failure behaviour.
- Warnings are errors for first-party code.
- Naming: types `PascalCase`, functions and variables `snake_case`, members `m_`,
  constants `k`, macros `ATLAS_UPPER_SNAKE`.
- Every first-party file starts with `// SPDX-License-Identifier: GPL-3.0-or-later`.

## Deferred work

Anything consciously not built goes in `docs/DEFERRED.md`, with the reason and what would
have to become true for it to be worth doing. A reason recorded only in a commit message is
a reason nobody can find.

## Before adding anything

- State the concrete problem an abstraction solves before writing it. Prefer plain code
  until two real call sites need the generalisation.
- Capture a benchmark or profiler trace before optimising. Report before and after.
- Ask before irreversible choices: public API shape, serialized file format, threading
  ownership, renderer backend boundary, scripting or plugin ABI, a new dependency.

## Completion checklist

A slice is done only when all of these hold:

1. It builds in the documented debug and release presets on this platform.
2. Relevant tests pass, and new behaviour has a test or a demonstrable app path.
3. `tools/precheck.sh` is clean: format, tidy, module deps, SPDX, tests.
4. Errors carry actionable context. Ownership and thread affinity are documented.
5. Profiling counters exist for performance-sensitive work.
6. Docs and ADRs match what was actually implemented.
7. No new warning, sanitizer error, data race, leak, fake success path, or unexplained
   TODO was introduced.
8. The report states files changed, commands run, test results, benchmark impact where
   relevant, remaining risks, and the next smallest slice.

## Forbidden shortcuts

- Do not delete a test, weaken an assertion, disable a warning globally, swallow an
  error, or comment out required behaviour to make something pass.
- Do not leave stubs that return success, empty catch blocks, or unreachable
  placeholder branches in a completed milestone.
- Do not claim a command ran without running it. Do not report results you did not see.
- Do not rewrite git history or run destructive git commands.
- Do not call the engine "optimised" from a frame rate, or "deterministic" because it
  uses a fixed timestep.
