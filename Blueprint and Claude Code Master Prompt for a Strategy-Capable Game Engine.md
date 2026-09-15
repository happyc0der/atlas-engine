# Blueprint and Claude Code Master Prompt for a Strategy-Capable Game Engine

## Recommended direction

Build a **strategy-capable engine platform**, not a universal engine and not the map game itself. The recommended first-year target is a C++23 engine for Windows and Linux with a clean simulation/render split, SDL3 for platform services and its modern GPU API, CMake presets, an editor/debug shell, deterministic test infrastructure, and profiling from the first milestone.

This direction preserves the long-term goal of a powerful reusable engine while forcing every early abstraction to prove itself in an executable “engine laboratory.” SDL3’s GPU API exposes modern command buffers, pipelines, transfer buffers, 3D rendering, and compute through a cross-platform interface modeled after Vulkan, Direct3D 12, and Metal, making it a more realistic solo-developer starting point than maintaining several native graphics backends immediately. CMake’s checked-in `CMakePresets.json` is designed to share repeatable project build configurations, while local settings belong in `CMakeUserPresets.json`.[^1][^2][^3][^4]

The architecture should be optimized for the actual hard part of a Paradox-like game: a large, deterministic, data-heavy world simulation that remains responsive while time advances quickly. Paradox’s own engineering presentation explains that simply applying `parallel_for` to entity collections only goes so far; its newer model separates read-heavy/private computation from a smaller ordered commit phase and enforces thread-friendly constraints through engine APIs. The prompt below therefore requires immutable render snapshots, explicit read/write declarations, staged mutations, deterministic commit order, replay hashes, and performance regression tests rather than vague promises of “multithreading.”[^5][^6][^7]

## Core architecture

| Area | Recommended decision | Reason |
|---|---|---|
| Language | C++23 | Strong native ecosystem, explicit memory/layout control, and a good fit for the project’s systems-programming goal. |
| Build | CMake + Ninja + checked-in presets | Makes Windows/Linux and CI builds reproducible; user-specific presets remain untracked.[^2][^3] |
| Platform/GPU | SDL3 + SDL_GPU behind engine-owned interfaces | Avoids starting with thousands of lines of Vulkan setup while retaining modern graphics and compute concepts.[^1][^8] |
| Scene representation | EnTT for presentation/scene entities; separate data-oriented simulation stores later | EnTT provides compact component pools, but its registry is not generally thread-safe, so it should not dictate the grand-strategy simulation’s concurrency model.[^9] |
| Tools UI | Dear ImGui docking branch | Its maintained docking branch supports dockable windows and multiple viewports, which suits an internal editor/debug shell.[^10][^11] |
| Profiling | Tracy from milestone zero | Tracy can inspect CPU/GPU timing, allocations, locks, context switches, and frame captures.[^12] |
| Scripting | Lua only after stable native APIs exist | Lua is lightweight and designed to be embedded through a C API, but introducing it before engine boundaries stabilize would multiply debugging surfaces.[^13] |
| Testing | Catch2 plus CTest, ASan/UBSan, and TSan where supported | ASan detects use-after-free and buffer errors; TSan detects data races, although both add runtime overhead and should use dedicated configurations.[^14][^15] |

A fixed simulation step should be independent of render frequency. The established accumulator pattern lets rendering produce elapsed time while simulation consumes it in fixed-size steps, with interpolation between previous and current presentation states. Fixed stepping alone does not guarantee cross-platform bit-identical results, so determinism must be checked with canonical state hashes, stable iteration order, seeded randomness, recorded commands, and explicit numeric policies.[^16][^17]

Data layout should follow measured access patterns instead of object hierarchies that look elegant on a diagram. Research comparing data-oriented variants found lower CPU time and cache-miss rates when iteration moved toward contiguous arrays, while EnTT likewise uses tightly packed component storage and warns that broad iteration and concurrent mutation need care. This supports a hybrid design: scene ECS for general engine objects, specialized structure-of-arrays tables and indices for future populations, provinces, markets, diplomacy, and AI.[^9][^18]

## Scope boundaries

The first engine release should provide reusable facilities, not a partially disguised grand-strategy game. Its proving application may render a synthetic colored region grid, select cells through an ID buffer, run a deterministic mock simulation, change speed, save/reload state, and show profiler counters. It must not contain countries, wars, diplomacy, economies, historical data, political borders, or game-specific scripting.

Do not begin with a custom allocator, custom ECS, custom scripting language, full render graph, networking stack, physics engine, animation system, or general-purpose plugin ABI. Each is a project-sized subsystem. Build a narrow vertical slice, benchmark it, and only replace a library or abstraction when a recorded limitation justifies the work.

## Copy-paste master prompt

Paste the following into Claude Code at the root of a new Git repository while using Plan Mode. Replace `Atlas` if another project name is preferred.

```text
You are the principal engine architect and implementation partner for a long-term C++ game-engine project named Atlas.

MISSION
Build a production-minded, high-performance engine platform that can later support a large map-based grand-strategy game. Build the engine first. Do not implement the game, game rules, historical content, countries, diplomacy, warfare, economics, or a Paradox clone.

The engine must eventually support:
- A large zoomable 2D/2.5D or 3D world map.
- Tens of thousands of logical regions and much larger data populations.
- Many independent simulation systems.
- Pause and multiple simulation speeds, including “run as fast as the machine permits.”
- Deterministic command processing, replay, save/load, and eventual multiplayer-friendly simulation.
- A responsive UI and renderer even when simulation ticks are expensive.
- Mod-friendly data and, later, safe embedded scripting.
- Strong profiling, benchmarking, debugging, and editor tools.

DEFAULT TECHNICAL DIRECTION
Use these defaults unless repository inspection proves one cannot work:
- C++23.
- CMake with Ninja and CMakePresets.json.
- Windows x64 and Linux x86_64 as tier-one platforms.
- Clang and MSVC as primary compilers; GCC compatibility where practical.
- SDL3 for windows, events, input, timing, and platform integration.
- SDL_GPU behind an Atlas-owned RHI/renderer boundary. Do not expose SDL types through public engine APIs outside the platform/render implementation modules.
- EnTT for scene/presentation entities only. Do not assume that future grand-strategy simulation data belongs in an ECS.
- Dear ImGui docking branch for internal editor and debug tooling.
- Tracy integration from the beginning, compiled out in normal release builds.
- Catch2 plus CTest for tests.
- vcpkg manifest mode with an explicitly pinned baseline, unless a written ADR demonstrates that another dependency manager is materially better.
- GitHub Actions for Windows and Linux CI.
- Clang-format and clang-tidy configurations checked into the repository.
- Authoring data in a readable, schema-versioned format; derived/cooked binary caches may be introduced after measurements justify them.
- Lua may be added later, only after the native engine API and serialization model stabilize.

OPERATING RULES
1. Begin in analysis and planning mode. Do not create implementation code until the initial architecture package is presented and approved.
2. Inspect the repository, operating system, installed compilers, SDKs, and available tools before proposing commands. Never pretend a command ran.
3. Use official documentation and primary sources for current APIs and versions. Pin dependency versions or commits; do not use floating branches in reproducible builds.
4. Work in small, compiling vertical slices. Never generate the entire engine in one pass.
5. End every implementation milestone with a clean configure, build, tests, sanitizer status where applicable, and a runnable demonstration.
6. Never “fix” a failure by deleting a test, weakening an assertion, disabling a warning globally, swallowing an error, or commenting out required behavior.
7. Do not leave fake implementations that return success, empty catches, unexplained TODOs, or unreachable placeholder branches in completed milestones.
8. Before adding an abstraction, state the concrete current problem it solves. Prefer plain code until two real call sites require generalization.
9. Before optimizing, capture a benchmark or profiler trace. After optimizing, report before/after results and verify behavior with tests.
10. Ask for approval before irreversible choices: public API shape, serialized file format, threading ownership, renderer backend boundary, scripting ABI, plugin ABI, or a major dependency.
11. Keep commits small and coherent. Suggest a commit message after each approved slice, but do not rewrite history or perform destructive Git operations.
12. Explain important systems concepts and tradeoffs concisely so the project owner can learn from the implementation.
13. If a requirement conflicts with correctness, portability, or measured evidence, stop and explain the conflict instead of guessing.

DOCUMENTATION TO CREATE BEFORE CODE
Create and present drafts of these files:
- README.md: project purpose, current status, exact quick-start commands, supported configurations, and non-goals.
- CLAUDE.md: fewer than 200 lines; exact build/test/format commands, architecture invariants, coding rules, completion checklist, and forbidden shortcuts.
- docs/PROJECT_CHARTER.md: goals, non-goals, users, target platforms, licensing assumptions, and definition of engine v0.1.
- docs/ARCHITECTURE.md: module graph, ownership, data flow, main loop, simulation/render split, threading model, and failure handling.
- docs/ROADMAP.md: milestone order, acceptance tests, exit criteria, risks, and deferred work.
- docs/PERFORMANCE.md: benchmark scenes, counters, trace workflow, baseline format, budgets, and regression policy.
- docs/DETERMINISM.md: tick model, command ordering, random-number policy, state hashing, numeric rules, replay format, and known limits.
- docs/DEPENDENCIES.md: each dependency, exact purpose, version/pin, license, update policy, and replacement boundary.
- docs/adr/0001-language-build-platform.md.
- docs/adr/0002-rendering-backend.md.
- docs/adr/0003-simulation-render-separation.md.
- docs/adr/0004-scene-ecs-vs-simulation-storage.md.
- docs/adr/0005-error-and-ownership-model.md.

For every ADR include context, decision, alternatives, consequences, rollback cost, and status. Explicitly compare raw Vulkan against SDL_GPU, and compare a single universal ECS against a scene-ECS plus specialized simulation stores.

PROPOSED REPOSITORY SHAPE
Use this only as a starting hypothesis and improve it during planning:

/
  CMakeLists.txt
  CMakePresets.json
  vcpkg.json
  CLAUDE.md
  README.md
  cmake/
  docs/
    adr/
  engine/
    core/
    platform/
    tasks/
    rhi/
    renderer/
    assets/
    scene/
    simulation/
    runtime/
    tools/
  apps/
    sandbox/
    editor/
    strategy_lab/
  tests/
    unit/
    integration/
    determinism/
  benchmarks/
  assets/
    source/
    test/
  tools/
  .github/workflows/

MODULE AND DEPENDENCY INVARIANTS
- core depends only on the C++ standard library and narrowly justified low-level libraries.
- platform may depend on core and SDL3.
- tasks may depend on core, never on scene, renderer, assets, or tools.
- rhi may depend on core and platform implementation details.
- renderer depends on rhi, core, math, and read-only render data; it does not mutate simulation state.
- assets owns virtual paths, asset IDs, loading state, dependency tracking, import/cook metadata, and hot reload.
- scene owns presentation entities and transforms; it is not the grand-strategy database.
- simulation owns generic tick scheduling, commands, snapshots, deterministic services, and system contracts; it contains no game rules.
- runtime composes engine modules.
- tools/editor may depend on runtime modules, but runtime modules never depend on editor code.
- applications contain composition roots and examples, not reusable engine logic.
- Public headers must not leak third-party implementation types unless an ADR explicitly permits it.
- Cyclic module dependencies are forbidden.

CODING AND OWNERSHIP POLICY
- Use RAII for every owned resource.
- Make ownership obvious: values and unique ownership by default; shared ownership requires a written reason.
- Use generation-counted handles for long-lived engine resources when raw pointers could become stale.
- Avoid hidden global mutable state and service locators.
- Avoid exceptions across module or thread boundaries. Use a project Result/Error type with context; exceptions inside third-party boundaries must be translated.
- Use std::span and views for non-owning ranges; do not copy bulk data accidentally.
- Avoid per-frame allocation in measured hot loops after warm-up.
- Avoid virtual dispatch in hot bulk-processing loops unless profiling shows it is irrelevant.
- Prefer stable IDs over serialized pointers or container positions.
- Use explicit-width integers in serialized formats and simulation state where width matters.
- Make logging structured and categorized. Fatal assertions are for violated programmer invariants, not recoverable user/data errors.
- Treat warnings as errors for first-party code, with narrowly scoped exceptions for third-party code.
- Every public API needs a concise contract documenting ownership, lifetime, thread affinity, and failure behavior.

MAIN LOOP AND TIME MODEL
Design three distinct notions of time:
- Real time for platform/UI responsiveness.
- Render time for interpolation and visual effects.
- Integer simulation ticks for authoritative state.

Required behavior:
- Rendering and UI must not define simulation correctness.
- Simulation advances in fixed logical ticks, not variable frame delta.
- Pause, single-step, normal speed, accelerated speed, and unbounded speed are scheduler policies controlling how many ticks are requested, not multipliers injected into game equations.
- Clamp catch-up work in interactive modes so the application cannot enter an unbounded spiral; report when the simulation falls behind.
- In unbounded/headless mode, skip rendering as needed and maximize simulation throughput.
- Renderer consumes immutable presentation snapshots and may interpolate visual state.
- Start v0.1 with a simple, correct implementation. Do not create a render thread until a trace proves the need and thread-affinity rules are documented.

SIMULATION AND CONCURRENCY CONTRACT
The future simulation must be thread-friendly by construction rather than retrofitted with random parallel loops.

Define a generic system model with:
- Stable system IDs and deterministic system order.
- Declared read sets and write sets at the resource/table level.
- A read/compute phase that may run concurrently and may write only to system-private scratch/output buffers.
- A commit phase that applies staged mutations in deterministic order.
- Explicit synchronization points.
- Deterministic reductions: no result may depend on worker completion order.
- A counter-based or otherwise reproducible RNG service with named streams; no ambient global RNG.
- Commands stamped with target tick, source, monotonic sequence, and validated payload.
- A canonical state hash for replay/desync detection.

Do not promise bit-identical determinism across compilers or CPU architectures without proving it. Initially guarantee replay determinism for the same supported build and platform. Document every source of nondeterminism: floating point, unordered containers, iteration order, clocks, filesystem order, random seeds, races, and serialization.

Do not build a sophisticated work-stealing scheduler in the first milestone. Start with a bounded thread pool or standard facilities behind a tiny task interface only when the first parallel benchmark exists. Keep deterministic simulation scheduling separate from general asynchronous jobs such as file I/O and asset decoding.

RENDERER CONTRACT
Build a modern but deliberately small renderer. The renderer must eventually support the map use case without containing map-game semantics.

Required abstractions, introduced only as needed:
- Device and feature/capability query.
- Swapchain/window target.
- Buffers, textures, samplers, shaders, pipelines, and fences through opaque generation handles.
- Command recording and submission.
- Upload/staging path.
- Render targets and depth targets.
- Explicit resource lifetime and deferred GPU destruction.
- Debug labels and GPU timing zones.
- Resize, minimize, device-loss, and shutdown handling.

The first renderer slice is a clear color and triangle. The second is an orthographic camera plus textured batched quads. Later slices add instancing, texture arrays, offscreen targets, an integer ID/picking target, frustum/chunk culling, LOD hooks, and debug overlays.

Do not begin with a general render graph. First implement two real multi-pass features. If resource-lifetime or pass-order complexity then justifies a graph, write an ADR and implement the minimum graph that solves the measured problem.

SHADERS
- Choose one canonical authoring language and document the cross-compilation pipeline to the formats required by supported SDL_GPU backends.
- Compile shaders as build/tool outputs, not at arbitrary runtime locations.
- Reflect or explicitly define resource layouts and validate them.
- Track shader source dependencies for development hot reload.
- A shader compile failure must preserve the last known-good runtime object and show an actionable error.

ASSET SYSTEM
Implement assets as IDs and handles, not direct filesystem paths in gameplay-facing APIs.

Required concepts:
- Mounted virtual filesystem with normalized UTF-8 virtual paths.
- Stable asset ID derived from canonical project metadata, not absolute machine paths.
- Source assets separated from imported/cooked artifacts.
- Versioned metadata and cache keys containing importer version and relevant settings.
- Asynchronous CPU loading/decoding with GPU upload finalized through the renderer’s legal thread/context.
- Placeholder/fallback resources for recoverable missing assets.
- Dependency graph and development hot reload.
- Clear state machine: unloaded, queued, loading, ready, failed, unloading.

Start with shader and texture assets only. Do not add a universal reflection/serialization framework during this slice.

SCENE AND ECS
Use EnTT to prove scene/presentation composition with a deliberately small component set:
- StableEntityId
- Name
- LocalTransform
- WorldTransform
- Hierarchy
- Sprite or MeshRenderData
- Camera

Requirements:
- Enforce hierarchy-cycle checks.
- Define transform update order.
- Serialize stable IDs and versioned component data, never raw EnTT entity values.
- Avoid putting renderer resource ownership directly inside components; use asset/resource handles.
- Make scene iteration and mutation phases explicit.
- Do not use this ECS as the default storage for future province, population, market, diplomacy, pathfinding, or AI tables. Those systems will select layouts from measured query patterns.

TOOLS AND EDITOR
The editor is an internal engineering tool first, not a polished commercial product.

Initial panels:
- Log console with severity/category filtering.
- Frame and simulation timing.
- Memory/allocation counters if available.
- Entity hierarchy and inspector.
- Asset status browser.
- Renderer resource/debug view.
- Simulation controls: pause, step, speed, reset, record replay, play replay.
- Determinism status: tick, seed, command count, and state hash.

Editor actions must use the same command/undo infrastructure intended for tools; direct arbitrary mutation from UI callbacks is forbidden when it would bypass validation.

OBSERVABILITY
Integrate instrumentation before optimization:
- Tracy frame markers, CPU zones, GPU zones when supported, lock contention, thread names, and important allocations.
- Structured logs with timestamps, thread IDs, category, severity, and source location in debug builds.
- Rolling counters for frame time, render submission, draw/dispatch counts, visible objects, upload bytes, asset queue depth, simulation tick time, system time, command count, and snapshot wait time.
- Headless benchmark output in machine-readable JSON plus a human-readable table.
- A baseline file tied to hardware, OS, compiler, build type, dependency revisions, and commit.

Never claim the engine is “optimized” from FPS alone. Report median and tail behavior, warm-up policy, scene size, tick count, build configuration, and machine metadata. Do not compare debug builds to release builds.

TEST STRATEGY
Create four layers:
1. Unit tests for IDs, handles, containers/utilities, paths, serialization, commands, time accumulation, RNG, and hashing.
2. Integration tests for application startup/shutdown, resource lifetime, asset loading, scene round trips, renderer smoke tests when a GPU is available, and headless fallback behavior.
3. Determinism tests that replay the same command log multiple times, vary legal worker counts, compare per-tick hashes, and identify the first divergent system/resource.
4. Benchmarks for iteration throughput, task overhead, snapshot publication, serialization, asset throughput, draw submission, picking readback, and synthetic simulation systems.

CI configurations:
- Windows MSVC debug and release.
- Linux Clang debug and release.
- Formatting check.
- clang-tidy on first-party targets.
- ASan + UBSan where supported.
- TSan as a separate Linux job where supported; do not combine it casually with ASan.
- Headless unit and determinism tests.
- Optional GPU smoke tests only on runners that actually expose the required GPU capability.

Every bug fix requires a regression test when reproducible.

ENGINE LABORATORIES
The applications are acceptance harnesses, not games.

Sandbox milestones:
- Open a window, process input, clear, and exit cleanly.
- Render a triangle.
- Render a textured quad and move an orthographic camera.
- Load assets through the asset system.
- Create, inspect, serialize, reload, and render a tiny scene.

Strategy Lab milestones:
- Generate a synthetic grid/chunk dataset from a fixed seed; do not use real geography.
- Render many colored cells in batches or instances.
- Pan and zoom smoothly.
- Select a cell using an integer ID render target and asynchronous or carefully bounded readback.
- Toggle synthetic map modes without rebuilding unrelated geometry.
- Run generic mock systems over data-oriented arrays at fixed ticks.
- Exercise pause, single-step, multiple speeds, unbounded headless execution, replay, save/load, and state hashes.
- Publish immutable render snapshots while simulation runs.
- Display profiler counters and identify bottlenecks.

Synthetic components may be named RegionValue, OwnerIndex, PopulationValue, AdjacencyIndex, or ColorIndex only as neutral test data. Do not add game mechanics.

PERFORMANCE POLICY
Do not invent performance claims. During planning, define provisional scenarios and measurement methods; establish numeric budgets only after the first baseline exists on the owner’s machine.

Include at least these scalable scenarios:
- 10k, 100k, and 1M simple data rows for iteration and mutation tests.
- 10k and 100k visible synthetic cells for renderer submission/culling tests where hardware permits.
- 1k, 10k, and 100k tick deterministic headless runs.
- Worker counts of 1, 2, 4, and hardware-concurrency minus one, clamped safely.

Track regressions against same-machine baselines. A benchmark failure should flag a review, not encourage manipulating the benchmark. Optimize likely map workloads through batching, culling, compact data, dirty propagation, cached derived data, and reduced synchronization only after traces show the relevant cost.

SAVE, REPLAY, AND VERSIONING
- Separate authoritative simulation state from presentation caches and GPU resources.
- Save files contain a magic value, format version, engine build/version, schema versions, integrity information, and deterministic state needed to resume.
- Use canonical ordering in serialized authoritative state.
- Replays contain initial-state identity, seed/stream data, tick-stamped commands, version metadata, and periodic hashes.
- Loading untrusted files must validate counts, lengths, versions, and bounds before allocation or indexing.
- Build migration support only after the first real schema change; document the policy now.

SECURITY AND ROBUSTNESS
Treat asset packs, mods, saves, shaders, and scripts as untrusted inputs:
- Validate lengths, integer conversions, indices, recursion depth, decompressed size, and paths.
- Prevent path traversal outside mounted roots.
- Never execute arbitrary code as part of asset import without an explicit trusted-tool boundary.
- Add fuzz targets later for custom parsers and binary loaders once those formats exist.
- Lua, when introduced, must have explicit exposed APIs, resource/time limits where practical, and no unrestricted filesystem or process access by default.

MILESTONE ORDER
M0 — Architecture and reproducible skeleton
Exit criteria:
- Approved docs and ADRs.
- Fresh configure/build/test works through documented presets.
- Core library, empty sandbox, test target, formatting, static analysis, CI, sanitizers, and Tracy option exist.
- Clean startup/shutdown with structured logs.

M1 — Platform loop
Exit criteria:
- SDL initialization, window, events, input state, clocks, resize/minimize handling, and clean shutdown.
- Unit-tested fixed-step accumulator independent from the renderer.
- Sandbox can run headless where supported.

M2 — Minimal GPU renderer
Exit criteria:
- Clear and triangle through the Atlas RHI boundary.
- Debug validation enabled in debug builds.
- Resize and resource shutdown tested.
- GPU/CPU profiling zones visible.

M3 — 2D camera and batching
Exit criteria:
- Orthographic camera, texture, sampler, quad batching/instancing, and debug overlay.
- No unbounded per-frame allocations after warm-up in the demonstrated path.
- Measured benchmark and machine-readable result.

M4 — Asset pipeline
Exit criteria:
- Virtual paths, asset IDs, shader/texture import, async CPU loading, GPU upload, failure fallbacks, and development hot reload.
- Round-trip and failure-path tests.

M5 — Scene and serialization
Exit criteria:
- Minimal scene ECS, transform hierarchy, stable IDs, versioned serialization, reload, and editor inspection.
- No simulation-domain assumptions in scene APIs.

M6 — Simulation kernel
Exit criteria:
- Fixed integer ticks, commands, stable scheduling, seeded RNG streams, state hashing, replay, save/load, and immutable render snapshots.
- Same command log produces the same per-tick hashes across repeated runs and supported worker-count variants, or limitations are precisely documented.

M7 — Strategy Lab
Exit criteria:
- Synthetic colored cell field, map-mode changes, panning/zooming, ID picking, chunk culling hooks, mock data-oriented systems, speed controls, replay, and profiling UI.
- Headless simulation benchmark runs independently of rendering.

M8 — Performance hardening
Exit criteria:
- Profile-guided changes only.
- Before/after traces and benchmark data.
- ASan/UBSan/TSan configurations clean for covered tests.
- Performance regression thresholds recorded for the owner’s machine and CI where stable.

M9 — Tooling and scripting decision
Exit criteria:
- Editor workflow is usable for test scenes and synthetic datasets.
- An ADR decides whether Lua is now justified, identifies its API boundary and security model, and either implements a tiny end-to-end script or explicitly defers it.

DEFINITION OF DONE FOR EVERY SLICE
A slice is done only when:
- It compiles in the documented debug and release presets on the active platform.
- Relevant tests pass.
- Errors include actionable context.
- Resource ownership and thread affinity are documented.
- New public behavior is demonstrated by an app or test.
- Profiling counters exist for performance-sensitive work.
- Documentation and ADRs reflect the final implementation.
- No new warning, sanitizer error, data race, leak, fake success path, or unexplained TODO was introduced.
- The response reports files changed, commands executed, test results, benchmark impact if relevant, remaining risks, and the next smallest slice.

CLAUDE CODE PROJECT INSTRUCTIONS
Create CLAUDE.md as concise persistent policy, not a duplicate of all design documents. Put directory-specific rules in .claude/rules/ only when path scoping saves context. Configure deterministic hooks only after the underlying commands work:
- After editing C/C++ files: run formatter on touched files.
- Before reporting completion: configure/build the affected preset and run relevant tests.
- Block destructive shell/Git commands unless explicitly approved.
- Never let a hook silently modify generated artifacts or vendor code.

Use specialized subagents for bounded research when useful: one for build/dependencies, one for renderer API, one for simulation/concurrency, and one for testing/tooling. They must return evidence and risks; the principal agent owns synthesis and prevents conflicting designs.

FIRST RESPONSE REQUIRED — NO IMPLEMENTATION YET
Return an “Architecture Gate 0” proposal containing:
1. Repository/environment findings.
2. Assumptions that need owner confirmation.
3. Module dependency diagram in Mermaid.
4. Main-loop and snapshot-flow diagram in Mermaid.
5. Thread ownership table.
6. Dependency matrix with alternatives and licenses.
7. Major ADR summaries.
8. Milestone plan with acceptance tests.
9. Top ten technical risks with mitigations.
10. Initial benchmark design.
11. Exact commands that M0 will support.
12. A list of files M0 will create or modify.
13. Questions limited to decisions that truly block M0.

Do not write engine implementation code in this first response. Wait for approval of Gate 0. After approval, implement M0 only, verify it completely, report the evidence, and wait before beginning M1.
```

## Why this prompt works

The prompt turns Claude Code into a gated engineering process rather than asking it to “make a game engine,” which would encourage broad scaffolding and shallow, untested abstractions. Anthropic recommends specific, concise, structured `CLAUDE.md` instructions, keeping the file under roughly 200 lines and moving narrower material into path-scoped rules or on-demand skills. Claude Code’s Plan Mode is specifically intended for reviewing a proposed approach before files are edited, and its hooks provide deterministic enforcement for formatting or validation tasks that should not depend on the model remembering to run them.[^19][^20][^21][^22]

The most important performance choice is the simulation contract, not a particular container or thread-pool implementation. Paradox’s published model maintains deterministic sequential entity handling within a system while allowing read-heavy portions of different systems to overlap, then applies visible state changes in a constrained write phase. The prompt adapts that idea without copying proprietary implementation details: systems declare data access, compute into private output, and commit through deterministic staging.[^7][^5]

The renderer is intentionally not raw Vulkan. SDL_GPU still exposes modern concepts—command buffers, pipelines, transfer buffers, fences, render passes, and compute—but lets the project concentrate on engine resource ownership, batching, map picking, and tooling instead of backend bring-up. Keeping an Atlas-owned boundary means a native Vulkan or Direct3D 12 backend can replace it later if profiling reveals a concrete limitation.[^1]

## Running the workflow

1. Create an empty repository, enter Claude Code’s Plan Mode, and paste the master prompt.
2. Review Gate 0 rather than approving automatically. Pay special attention to the module dependency graph, simulation mutation rules, GPU resource lifetime, and what Claude proposes to defer.
3. Approve only M0. Require real command output for configure, build, tests, and CI configuration before moving to M1.
4. Start a fresh Claude Code conversation for each milestone or major subsystem. Persistent project rules belong in `CLAUDE.md`; design details belong in versioned docs and ADRs, not an ever-growing chat context.[^23][^19]
5. Use the Strategy Lab as the architecture’s truth test. If a general abstraction does not make the lab clearer, faster, safer, or easier to inspect, it probably does not belong yet.

## Review warnings

Reject a Gate 0 plan if it proposes a custom ECS, allocator, job system, scripting language, physics engine, renderer graph, editor, networking layer, and asset format simultaneously. Also reject claims that the engine is deterministic merely because it uses a fixed timestep, or optimized merely because it is multithreaded. ThreadSanitizer can detect races but commonly imposes substantial slowdown, so it belongs in a dedicated validation configuration rather than the everyday interactive build.[^15]

Do not let the eventual map game disappear from architectural tests, but do not put game rules into the engine to anticipate it. The right compromise is a synthetic strategy laboratory that proves rendering, picking, data iteration, snapshot publication, fixed ticks, replay, and accelerated headless simulation before real game content exists.

---

## References

1. [SDL/include/SDL3/SDL_gpu.h at main · libsdl-org/SDL · GitHub](https://github.com/libsdl-org/SDL/blob/main/include/SDL3/SDL_gpu.h) - Simple DirectMedia Layer. Contribute to libsdl-org/SDL development by creating an account on GitHub.

2. [cmake-presets.7.rst.txt](https://cmake.org/cmake/help/latest/_sources/manual/cmake-presets.7.rst.txt)

3. [cmake-presets(7) — CMake 4.4.3 Documentation](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html)

4. [Modern GPU API | libsdl-org/SDL | DeepWiki](https://deepwiki.com/libsdl-org/SDL/2.4-modern-gpu-api) - The SDL Modern GPU API provides a cross-platform abstraction layer for interacting with modern graph...

5. [Multi Threading Model in Paradox Games - ACCU 2023](https://accu.org/conf-docs/PDFs_2023/XMultiThreadingModelinParadoxGamesPastPresentandFuture.pdf)

6. [Multi Threading Model in Paradox Games - Mathieu Ropert](https://www.youtube.com/watch?v=e_2z7uWouuk) - In this talk we will see how the threading model of the game simulation evolved over time to try and...

7. [Multi Threading Model in Paradox Games: Past, Present and Future - Mathieu Ropert - ACCU 2023](https://www.youtube.com/watch?v=M6rTceqNiNg) - ACCU Membership: https://tinyurl.com/ydnfkcyn
https://accu.org
https://www.accuconference.org/ 

Mul...

8. [SDL3/APIByCategory](https://wiki.libsdl.org/SDL3/APIByCategory) - The Simple Directmedia Layer Wiki

9. [Entity Component System · skypjack/entt Wiki](https://github.com/skypjack/entt/wiki/Entity-Component-System) - For performance reasons, EnTT favors storage compaction in all cases, although often accessing a com...

10. [Docking and Viewports - Dear ImGui](https://ocornut-imgui.mintlify.app/advanced/docking-viewports)

11. [Docking · ocornut/imgui Wiki · GitHub](https://github.com/ocornut/imgui/wiki/Docking) - Dear ImGui: Bloat-free Graphical User interface for C++ with minimal dependencies - ocornut/imgui

12. [GitHub - wolfpld/tracy: Frame profiler](https://github.com/wolfpld/tracy) - Frame profiler. Contribute to wolfpld/tracy development by creating an account on GitHub.

13. [Lua 5.4 Reference Manual](https://www.lua.org/manual/5.4/manual.html) - Lua is a powerful, efficient, lightweight, embeddable scripting language. It supports procedural pro...

14. [AddressSanitizer · google/sanitizers Wiki](https://github.com/google/sanitizers/wiki/addresssanitizer) - AddressSanitizer (aka ASan) is a memory error detector for C/C++. This tool is very fast. The tool w...

15. [ThreadSanitizer — Clang 24.0.0git documentation](https://clang.llvm.org/docs/ThreadSanitizer.html) - ThreadSanitizer is a tool that detects data races. It consists of a compiler instrumentation module ...

16. [Fix Your Timestep!](https://gafferongames.com/post/fix_your_timestep/) - Here's how to do it. Advance the physics simulation ahead in fixed dt time steps while also making s...

17. [Deterministic Lockstep](https://gafferongames.com/post/deterministic_lockstep/) - Introduction Hi, I’m Glenn Fiedler and welcome to Networked Physics. In the previous article we expl...

18. [Investigating the effect of implementing Data-Oriented ...](https://www.diva-portal.org/smash/get/diva2:1578616/FULLTEXT01.pdf)

19. [How Claude remembers your project - Claude Code Docs](https://docs.anthropic.com/en/docs/claude-code/memory) - Give Claude persistent instructions with CLAUDE.md files, and let Claude accumulate learnings automa...

20. [Extend Claude Code - Claude Code Docs](https://docs.anthropic.com/en/docs/claude-code/features-overview) - This guide covers the extension layer: features you add to customize what Claude knows, connect it t...

21. [Common workflows - Claude Code Docs](https://docs.anthropic.com/en/docs/claude-code/common-workflows) - This page collects short recipes for everyday development. For higher-level guidance on prompting an...

22. [Automate actions with hooks - Claude Code Docs](https://docs.anthropic.com/en/docs/claude-code/hooks-guide) - To test the hook, ask Claude to add a line with single-quoted strings to a JavaScript file, then ope...

23. [Overview - Claude Code Docs](https://docs.anthropic.com/en/docs/claude-code) - Claude Code is an AI-powered coding assistant that helps you build features, fix bugs, and automate ...

