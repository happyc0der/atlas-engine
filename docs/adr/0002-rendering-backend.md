# 0002 — Rendering backend: SDL_GPU behind an engine-owned boundary

## Status

Accepted, 2026-09-14.

## Context

Atlas must eventually draw a large zoomable world map: many thousands of instanced cells,
map-mode switching, integer-ID picking through a render target, offscreen passes, and
culling. It targets three platforms whose native graphics APIs are different: Metal on
macOS, Vulkan on Linux, Direct3D 12 on Windows.

The development machine has an Apple M4 Pro with Metal 4, no Vulkan SDK, no MoltenVK, and
only Command Line Tools, so there is no `metal` shader compiler available.

The relevant question is not which API is most powerful. It is where a solo developer's
time goes. Writing and maintaining three native backends is a project in itself, and it is
not the part of the project that teaches anything about the actual hard problem, which is a
large deterministic simulation feeding a responsive renderer.

## Decision

**Use SDL_GPU, behind an engine-owned `atlas::rhi` boundary.**

SDL_GPU exposes modern explicit-graphics concepts through one cross-platform interface:
command buffers, render and copy passes, graphics pipelines, transfer buffers, fences, and
compute. It maps onto Vulkan, Direct3D 12, and Metal natively. On macOS it uses Metal
directly, so no Vulkan translation layer is involved.

The boundary is the point of the decision. `atlas::rhi` exposes opaque generation-counted
handles for buffers, textures, samplers, shaders, and pipelines, plus move-only RAII scopes
for frames and render passes. Public headers are limited to handles, types, descriptors,
and the device. Every SDL type stays inside `engine/rhi/src`; `SDL_Window*` reaches the RHI
only through a dedicated internal target whose sole permitted consumer is `atlas::rhi`.

**Shaders.** HLSL is the canonical authoring language, compiled offline to SPIR-V for
Vulkan, DXIL for Direct3D 12, and MSL for Metal, with reflection metadata alongside. Metal
shaders ship as **MSL source text**, which SDL_GPU compiles at device creation; this is what
makes the missing `metal` compiler a non-issue. Cooked outputs are tracked in the
repository so that building and running Atlas never requires the shader toolchain.

**Debug and validation.** Devices are created with debug mode enabled in debug builds.
Debug groups and object names are used throughout. GPU-side timing is not available: SDL_GPU
exposes fences but no timestamp queries, so "GPU zones" means CPU-side acquire, record, and
submit timing plus fence latency, and this is stated wherever GPU cost is reported.

**Resource destruction.** SDL_GPU already defers destruction until the GPU has finished
with a resource, so `destroy` bumps the handle generation and releases immediately. A
frame-indexed deletion queue is added only if a future backend needs one.

**No render graph.** Two real multi-pass features must exist first. If resource lifetime or
pass ordering then justifies a graph, that is a new ADR and a minimum implementation.

## Alternatives

**Raw Vulkan, with MoltenVK on macOS.** Maximum control and the most transferable
knowledge, at the cost of thousands of lines of setup before a triangle appears, plus a
translation layer on the primary development machine and a second backend for Windows
eventually anyway. The map workload needs batching, instancing, culling, and picking, none
of which require features SDL_GPU lacks. Rejected as the starting point, explicitly
preserved as the replacement path.

**bgfx** is mature and covers more backends, including older ones. Its abstraction is
closer to Direct3D 11 in spirit, which is further from where the project wants to end up,
and it is a much larger dependency to keep private behind a boundary. Rejected.

**Diligent Engine** is capable but a larger framework, with more opinions about how an
engine should be structured than Atlas wants to inherit. Rejected.

**OpenGL** would be the simplest bring-up and is deprecated on macOS. Rejected.

## Consequences

- A triangle appears in weeks rather than months, and the time saved goes into the
  simulation and tooling, which is where the project's actual risk lives.
- Atlas is limited to SDL_GPU's feature set: no bindless resources, no ray tracing, no
  timestamp queries. None are needed for a 2D map, and each would be a reason to revisit.
- SDL_GPU is younger than the native APIs; bugs are more likely to be in the abstraction.
  The boundary means a workaround stays in one file.
- Shader authoring depends on a toolchain that has no prebuilt macOS binaries. Cooked
  outputs are tracked so this never blocks a build, and the toolchain question is settled
  separately in ADR-0006.
- Debugging is weaker than with a native backend: no Xcode frame capture without full
  Xcode, no RenderDoc through SDL_GPU's Metal path. Metal's validation layers are available
  through environment variables and are enabled in GPU test presets.

## Rollback cost

Medium, and bounded by the boundary. Replacing SDL_GPU with a native backend means
reimplementing `engine/rhi/src` against the same handles, descriptors, and frame scopes,
plus a shader cooking target for the new format. No module above `rhi` should need to
change. If the boundary has leaked by then, the cost is much higher, which is exactly why
the leak check is mechanical rather than a matter of discipline.
