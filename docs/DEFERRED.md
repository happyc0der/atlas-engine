<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Deferred work

Everything consciously not built, and why. One list, because the reasons were previously
spread across three separate paragraphs of the roadmap, the charter and several decision
records, and a reason nobody can find is a reason nobody can challenge.

A deferral is not a to-do. Each entry says what would have to become true for the work to be
worth doing; several will never become true, and that is a fine outcome.

Last reviewed 2026-09-15, after M6.

## Open gaps in the infrastructure

These were specified in the Gate 0 plan and are simply not done. They are the only entries
here without a considered reason.

| What | Why it matters | Found |
|---|---|---|
| A software-rasteriser smoke job | The renderer is verified on exactly one machine. This would give it automatic coverage on Linux without a graphics processor. | M0–M4 audit |
| The Stop hook | The plan specified three hooks and two were written. The missing one runs the precheck before a change is called finished. | M0–M4 audit |
| `tools/format.ps1` | Windows is tier-one and its contributors have no formatting entry point. | M0–M4 audit |

The planned `shaders.yml` continuous-integration job is **not** on this list. Its purpose,
catching a shader edited without re-cooking, is served by the `shaders_current` test, which
runs everywhere the toolchain exists and skips itself where it does not. That is a
substitution rather than a gap, recorded here because it was never recorded anywhere else.

## Never verified, as distinct from deferred

| What | State |
|---|---|
| Continuous integration | Has never executed. There is no git remote, so the three workflow files have been reviewed and validated as YAML and never run. |
| Windows | Has never been compiled, by anything. The build is configured for it and that configuration is untested. |
| x86_64 | No machine. The golden-hash comparison covers two toolchains on arm64 only, which is a real result about compilers and says nothing about architectures. |
| A decision the plan asked for | The plan said the owner would decide by M3 whether to acquire Windows hardware or a virtual machine. M3 passed without the question being put. |

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

- **A render graph.** Waits for two real multi-pass features to exist, so it is designed
  against something rather than imagined.
- **Rotated sprites.** The batcher draws axis-aligned rectangles. A scene can express a
  rotation the renderer cannot draw, and the sandbox scene avoids rotation rather than
  displaying something that does not match what it holds. Picked up when the renderer next
  changes, in M7.

### M4 — assets

- **An asset status panel.** The overlay's only consumer reports asset counts in its exit
  summary, and a panel would be a second place for the same three numbers to be wrong.
- **Shader loading through the registry.** Shaders still come from the generated manifest. The
  registry has nothing to add to a shader whose resource counts are compile-time constants.
- **Cooked-artifact caching.** Nothing yet takes long enough to import to justify a cache.
- **An asset dependency graph.** With one asset type, nothing depends on anything.

### M5 — scene

- **The `runtime` module.** Planned here on the assumption a second application would need the
  same composition. Scene inspection went into the existing overlay, so there is still one
  composition root, and extracting a module for it would be an abstraction with a single call
  site. Created when a second application genuinely needs it.
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

## Deferred by the charter, until a recorded limitation justifies the work

The `tasks` module and its worker pool, created with the first parallel benchmark in M8. A
custom allocator. A custom entity-component system. A work-stealing scheduler. Networking.
Physics. Animation. A plugin interface. Multi-viewport overlay windows. Filesystem watchers,
in place of which hot reload polls modification times. Audio, gamepad input, input method
editors, and localisation.

Each of these is a project-sized subsystem. The bar is a measured limitation in the thing
being built, not an expectation that one will appear.
