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
ctest --preset macos-debug-gpu        # test including GPU tests (M2+, real GPU only)
cmake --workflow --preset ci-macos-debug   # configure + build + test

tools/format.sh --check               # clang-format, --fix to apply
tools/tidy.sh macos-debug             # clang-tidy over first-party targets
python3 tools/check_module_deps.py    # module boundary + cycle + exception check
python3 tools/check_spdx.py           # licence headers
tools/precheck.sh macos-debug         # everything above, in order
tools/ci/docker_linux.sh              # Linux build in a container, before pushing
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
```

Sanitizers: presets `macos-asan`, `macos-tsan`, `linux-clang-asan`, `linux-clang-tsan`.
Never combine ASan and TSan. TSan runs only `unit` and `determinism` labelled tests.

## Architecture invariants

- Module dependencies are declared in `cmake/ModuleGraph.cmake` and enforced at configure
  time. Adding an edge means editing that table, with a reason.
- Third-party libraries are `PRIVATE` links. They must not appear in any public header
  except where an ADR allows it (Tracy in `core/profile.hpp`, EnTT in `scene`).
- SDL types live only in `engine/platform/src`, `engine/rhi/src`, and the
  `atlas::platform_internal` target. Never in `include/`.
- `renderer` reads immutable snapshots; it never mutates simulation or scene state.
- `scene` is presentation only. It is not the grand-strategy database.
- `simulation` contains tick scheduling, commands, hashing, and system contracts.
  It contains no game rules.
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
- SDL types stay in `engine/platform/src` and `engine/rhi/src`. The native window handle
  reaches the renderer only through `atlas::platform_internal`.
- Shaders are authored in HLSL and cooked to SPIR-V and MSL. Commit the cooked outputs.
- No per-frame allocation in measured hot loops after warm-up.
- Assertions are for violated programmer invariants. Recoverable user or data errors
  return an `Error` with context.
- Every public API documents ownership, lifetime, thread affinity, and failure behaviour.
- Warnings are errors for first-party code.
- Naming: types `PascalCase`, functions and variables `snake_case`, members `m_`,
  constants `k`, macros `ATLAS_UPPER_SNAKE`.
- Every first-party file starts with `// SPDX-License-Identifier: GPL-3.0-or-later`.

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
