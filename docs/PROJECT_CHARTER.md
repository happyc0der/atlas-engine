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
- A universal engine. There is no plan for physics, for a general-purpose plugin ABI for
  native code, or for a custom scripting language. Networking means deterministic lockstep
  over the command queue and nothing else. Animation is presentation-side and never enters
  authoritative state. Player mods are untrusted scripts behind the command boundary, not
  loadable libraries. Amended by [ADR-0010](adr/0010-charter-amendment.md).
- A polished commercial editor. The editor is an internal engineering tool.
- Audio, gamepad input, IME and localisation were out of scope for v0.1, which shipped at
  M7. Each is a planned milestone under [ADR-0010](adr/0010-charter-amendment.md), built
  against the strongest consumer that exists at the time, and any of them may be dropped
  without amending this charter again.
- Device-loss *recovery*. Device loss is detected and reported with an actionable error.

Deferred until a recorded limitation justifies the work: custom allocator, custom ECS,
render graph, work-stealing scheduler, native Vulkan or D3D12 backend, physics. Physics'
trigger is a consumer that needs bodies interacting through forces rather than through
commands; a map-based strategy game has none. Embedded scripting is decided by ADR-0015.

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
cross-platform *bit-identical simulation* is not claimed. See DETERMINISM.md. ADR-0014
permits a lockstep session between different builds only when both peers' golden hashes agree
at handshake; that is a probe, not a claim.

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

## After v0.1

v0.1 was declared against the list above, item by item, which is what made the declaration mean
something. There is no such list for what follows it.

On 2026-09-17 the owner decided to extend the engine before starting the game, with seven
subsystems: animation, networking, sandboxed mods, audio, gamepad input, IME and localisation.
[ADR-0010](adr/0010-charter-amendment.md) records that decision, what it changed in this
document, and the risk it accepts.

**Completing those seven is not v1.0.** They were chosen by decision rather than derived from a
requirement, so finishing them proves only that they were built. **v1.0 is declared when a game
project links the engine, loads data, and runs a deterministic simulation without patching
engine internals** — user 2 above — because that is the only test of whether these were the
right seven.
