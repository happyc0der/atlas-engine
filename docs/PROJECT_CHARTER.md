# Project charter

## Purpose

Atlas is a reusable C++23 engine platform whose architecture is driven by one demanding
workload: a large, deterministic, data-heavy world simulation that stays responsive while
simulated time advances quickly. That workload belongs to a future map-based
grand-strategy game which is **not** part of this project.

The engine is proven by a synthetic "strategy laboratory" application rather than by game
content. If an abstraction does not make that laboratory clearer, faster, safer, or easier
to inspect, it does not belong in the engine yet.

## Goals

1. A clean simulation/render split: fixed integer simulation ticks, immutable presentation
   snapshots, and a renderer that never reads authoritative state.
2. Deterministic command processing, replay, save/load, and canonical state hashing.
3. A small modern renderer that can eventually draw a large zoomable world map with
   integer-ID picking, batching, and culling.
4. Data layouts chosen from measured access patterns, not from object hierarchies.
5. Profiling, benchmarking, and regression tracking present from the first milestone.
6. Internal engineering tools: log console, timing, inspection, simulation controls.
7. Reproducible builds on three platforms from checked-in presets and pinned dependencies.

## Non-goals

- Any game rule or content: countries, wars, diplomacy, economies, populations as a game
  concept, historical data, political borders, or game-specific scripting.
- A universal engine. There is no plan for physics, animation, networking, a
  general-purpose plugin ABI, or a custom scripting language.
- A polished commercial editor. The editor is an internal engineering tool.
- Audio, gamepad input, IME, and localisation are out of scope for v0.1.
- Device-loss *recovery*. Device loss is detected and reported with an actionable error.

Deferred until a recorded limitation justifies the work: custom allocator, custom ECS,
render graph, work-stealing scheduler, native Vulkan or D3D12 backend, embedded Lua.

## Users

1. **The project owner**, as engine developer and sole maintainer.
2. **A future game project** built on top of the engine, which must be able to link the
   engine, load data, and run a deterministic simulation without patching engine internals.
3. **CI**, which must be able to build and test everything headlessly.

## Target platforms

| Platform | Role |
|---|---|
| macOS 15+ arm64, Apple clang 21 | Primary development; the only platform with GPU verification on real hardware |
| Linux x86_64, Clang 19+ | Tier one; headless CI plus a software-Vulkan (lavapipe) GPU lane |
| Windows x64, MSVC 19.4x | Tier one; headless CI. GPU path unverified until Windows hardware exists |

GCC is expected to work and is not a CI gate. Cross-platform *behaviour* is a CI claim;
cross-platform *bit-identical simulation* is not claimed. See DETERMINISM.md.

## Licensing

Atlas is GPL-3.0-or-later. Every first-party file carries an SPDX identifier, checked by
`tools/check_spdx.py`.

Every dependency must be licence-compatible with GPLv3 distribution, recorded in
DEPENDENCIES.md with its licence and pin. Current dependencies are permissive
(zlib, MIT, BSD-3-Clause, BSL-1.0) and therefore compatible. A dependency whose licence is
incompatible is not adoptable, regardless of technical merit.

## Definition of engine v0.1

Engine v0.1 is reached when milestone M7 passes its exit criteria. It must provide:

- A reproducible build and test workflow on all three tier-one platforms.
- Window, input, events, clocks, and clean startup and shutdown.
- An engine-owned renderer boundary over SDL_GPU: buffers, textures, samplers, shaders,
  pipelines, render passes, uploads, and integer-ID readback.
- An asset system with virtual paths, stable asset IDs, async CPU loading, cooked-artifact
  caching, fallbacks, and development hot reload, for shaders and textures.
- A scene layer for presentation entities with transform hierarchy and versioned
  serialization using stable IDs.
- A simulation kernel: fixed integer ticks, commands, deterministic system ordering,
  seeded RNG streams, canonical state hashing, replay, save/load, and snapshot publication.
- A strategy laboratory demonstrating a synthetic cell field with map modes, pan and zoom,
  ID picking, mock data-oriented systems, speed controls including unbounded headless
  execution, replay, and profiler counters.
- Documentation and ADRs that match the implementation.

Explicitly **not** required for v0.1: parallel simulation execution (M8), a scripting
layer (M9), and any game semantics (never).
