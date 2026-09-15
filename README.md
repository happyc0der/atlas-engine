# Atlas

Atlas is a C++23 engine platform being built to eventually support a large, map-based
grand-strategy game. It is an **engine**, not a game: it contains no countries, wars,
diplomacy, economies, historical data, or game rules, and it is not intended to.

The project is developed in gated milestones. Each milestone ends with a clean
configure, build, test run, and a runnable demonstration before the next one starts.

## Current status

**Milestone M0 — architecture and reproducible skeleton.** The engine currently starts,
logs, and exits. There is no window, renderer, asset system, scene, or simulation yet.
See [docs/ROADMAP.md](docs/ROADMAP.md) for what arrives when.

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

./build/macos-debug/bin/atlas_sandbox --headless --iterations 100
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
there is no plan for a physics engine, an animation system, a networking stack, a
general-purpose plugin ABI, or a custom scripting language. Audio, gamepad input, IME,
and localisation are out of scope for v0.1.

Sub-systems are added when a real call site needs them, never in anticipation.

## Documentation

- [docs/PROJECT_CHARTER.md](docs/PROJECT_CHARTER.md) — goals, non-goals, definition of v0.1
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — modules, data flow, main loop, threading
- [docs/ROADMAP.md](docs/ROADMAP.md) — milestones and exit criteria
- [docs/PERFORMANCE.md](docs/PERFORMANCE.md) — benchmarks, counters, regression policy
- [docs/DETERMINISM.md](docs/DETERMINISM.md) — tick model, hashing, numeric rules, limits
- [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) — every dependency, pin, and licence
- [docs/adr/](docs/adr/) — architecture decision records

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).
