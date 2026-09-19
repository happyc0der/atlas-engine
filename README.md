# Atlas

Atlas is a C++23 engine platform being built to eventually support a large, map-based
grand-strategy game. It is an **engine**, not a game: it contains no countries, wars,
diplomacy, economies, historical data, or game rules, and it is not intended to.

The project is developed in gated milestones. Each milestone ends with a clean
configure, build, test run, and a runnable demonstration before the next one starts.

## Current status

**Sixteen milestones are done, M0 through M15.** Engine v0.1 was declared at M7 against the
charter item by item. M10 amended the charter to plan seven subsystems it had excluded; M11
through M15 built the first five.

What exists: a windowing and input layer; a renderer over SDL_GPU with offscreen targets,
integer-identifier picking and non-stalling streaming uploads; an asset pipeline with virtual
paths, background loading, hot reload and a cooked-artifact cache; a scene layer with a
transform hierarchy and versioned serialization; a deterministic simulation kernel with fixed
integer ticks, commands, seeded random streams, canonical state hashing, replay, and save and
load; a worker pool the simulation's compute phase runs on; an undoable edit layer; text, input-method
and gamepad input; audio with a mixer, voices and loadable clips; animation from clip files,
composed on top of what a person authored rather than into it; deterministic lockstep over the
command queue, proved by several simulations in one process agreeing hash for hash through the
real peer interface; sandboxed mods as untrusted WebAssembly that reaches simulation state only
by submitting commands, with a guest interface that has no clock, no filesystem and no generator
of its own; and an engineering overlay with scene, log, asset, timing and control panels
that can be typed into.

Two applications drive it. `atlas_sandbox` is the lifecycle and scene demonstration.
`atlas_lab` is the Strategy Laboratory: a synthetic million-cell grid with map modes, picking,
time controls and replay, which exists to test the architecture rather than to be a game.

**What is not done.** The renderer has been verified on one graphics processor and one software
rasteriser; Direct3D 12 and non-Apple hardware are unverified, and that is the largest untested
surface in the project. **There is no network transport** — M14 designed lockstep and proved it
over an in-memory link, by decision, and choosing a transport is a separate change with recorded
criteria. Mods exist but nothing large has been written as one: the demonstration mod is under three
hundred bytes, so what loading a real one costs is unmeasured, and the lab runs one mod at a
time. [ADR-0015](docs/adr/0015-sandboxed-mods.md) chose WebAssembly over Lua and Luau, and
records what the overlay port it needed has cost so far. Everything
consciously not built is listed with its reason in [docs/DEFERRED.md](docs/DEFERRED.md).

See [docs/ROADMAP.md](docs/ROADMAP.md) for what each milestone did and what it cost.

## Quick start

Requirements: a C++23 compiler (Apple clang 21+, Clang 19+, or MSVC 19.4x), CMake 3.28+,
Ninja, `pkg-config`, and Git.

```sh
git clone --recurse-submodules <repository-url> atlas
cd atlas
./external/vcpkg/bootstrap-vcpkg.sh -disableMetrics   # bootstrap-vcpkg.bat on Windows

cmake --preset macos-debug          # or linux-clang-debug, windows-msvc-debug
cmake --build --preset macos-debug
ctest --preset macos-debug

./build/macos-debug/bin/atlas_sandbox --headless --ticks 100
```

Something to look at:

```sh
./build/macos-debug/bin/atlas_sandbox --scene            # a scene, an overlay; Escape quits
./build/macos-debug/bin/atlas_lab --grid 512             # the Strategy Laboratory
./build/macos-debug/bin/atlas_lab --headless --grid 1024 --ticks 1000   # no window at all
```

`cmake --workflow --preset ci-macos-debug` runs configure, build, and test in one step.

The first configure builds the pinned dependencies from source and takes a few minutes.
Later configures reuse the binary cache in `.cache/vcpkg-archives`.

## Supported configurations

| Platform | Compiler | Status |
|---|---|---|
| macOS 15+ arm64 | Apple clang 21 | Tier one: developed and GPU-verified on real hardware |
| Linux x86_64 | Clang 19+ | Tier one: verified headless in CI, plus a software-GPU lane |
| Windows x64 | MSVC 19.4x (VS 2022 17.14+ / VS 2026) | Tier one: verified headless in CI; GPU path not yet verified |

GCC 14+ works and is not gated in CI. Clang 18 and earlier cannot build Atlas: they
cannot compile `std::expected` against libstdc++. See docs/DEPENDENCIES.md.

## Non-goals

Atlas will not contain game rules or content of any kind. It is not a universal engine:
there is no plan for a physics engine, a plugin ABI for native code, or a custom scripting
language. Networking means deterministic lockstep over the command queue; mods are sandboxed
scripts behind that same boundary; gamepad input and input methods arrived in M11, audio in M12,
animation in M13 and lockstep networking in M14; and localisation is a planned milestone under
[ADR-0010](docs/adr/0010-charter-amendment.md). Animation is presentation: it writes a derived
pose, reaches no authoritative state, and is hashed nowhere.

Sub-systems are added when a real call site needs them, never in anticipation.

## Documentation

- [docs/PROJECT_CHARTER.md](docs/PROJECT_CHARTER.md) — goals, non-goals, definition of v0.1
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — modules, data flow, main loop, threading
- [docs/ROADMAP.md](docs/ROADMAP.md) — milestones and exit criteria
- [docs/PERFORMANCE.md](docs/PERFORMANCE.md) — benchmarks, counters, regression policy
- [docs/DETERMINISM.md](docs/DETERMINISM.md) — tick model, hashing, numeric rules, limits
- [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) — every dependency, pin, and licence
- [docs/adr/](docs/adr/) — architecture decision records
- [docs/reports/](docs/reports/) — milestone reports: what changed, what was run, what is still risky
- [docs/DEFERRED.md](docs/DEFERRED.md) — everything consciously not built, and what would change that

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).
