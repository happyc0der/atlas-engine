# Dependencies

Every dependency is pinned. Versions come from vcpkg manifest mode with a fixed
`builtin-baseline`, plus explicit `overrides` so that moving the baseline never silently
changes a version. vcpkg itself is a git submodule at `external/vcpkg`, pinned to the
commit below, so a fresh clone reproduces the dependency set with no environment variables.

Atlas is GPL-3.0-or-later. Every dependency must be licence-compatible with GPLv3
distribution. All current dependencies are permissive and therefore compatible.

## Toolchain pins

| Tool | Pin | Notes |
|---|---|---|
| vcpkg | release 2026.07.29, commit `9e593bb18ea69cc5095e012465dcd675a822ed0d` | Submodule at `external/vcpkg`; the commit the tag resolves to, not the tag object |
| Ninja | 1.13.2 | Present on all three CI images |
| Clang (Linux) | 19 minimum | Clang 18 cannot compile `std::expected` with libstdc++: it reports `__cpp_concepts=201907` and libstdc++ requires `202002`. Both clang-19 and libstdc++-14 are in the Ubuntu 24.04 repositories |
| CMake | 3.28 minimum, presets schema 8 | The floor is plain Ubuntu 24.04, whose apt CMake is 3.28.3. Schema 8 covers every preset feature used, workflow presets included |

## Library dependencies

| Library | Purpose | Pin | Licence | GPLv3-compatible | Boundary | Added |
|---|---|---|---|---|---|---|
| Catch2 | Unit and integration test framework | 3.15.3 | BSL-1.0 | Yes | Test targets only | M0 |
| Tracy | Frame profiler client | 0.13.1 | BSD-3-Clause | Yes | `atlas/core/profile.hpp` macros, behind `ATLAS_PROFILE`; vcpkg feature `profile` | M0 |
| SDL3 | Window, events, input, GPU | 3.4.12 | zlib | Yes | Private to `platform` and `rhi` | M1 |
| glslang | Compiles HLSL to SPIR-V | 16.4.0 (vcpkg) / 16.6.0 (Homebrew) / whatever `glslang-tools` provides on the CI image. **Three builds, and their SPIR-V is not byte-identical for a non-trivial shader** — see PERFORMANCE.md under M13 | BSD-3-Clause and Apache-2.0 | Yes | Build-time tool only; never linked into engine targets | M2 |
| SPIRV-Cross | Translates SPIR-V to Metal Shading Language | 1.4.350.1 (vcpkg) / 1.4.357.0 (Homebrew) | Apache-2.0 | Yes | Build-time tool only | M2 |
| stb | Image decoding (`stb_image`). The port also installs `stb_vorbis.c` v1.22 on every triplet; Atlas does not compile it, and M12 deferred Ogg support. | 2024-07-29, port-version 1 | MIT / Unlicense | Yes | Private to the assets importer | M4 |
| Dear ImGui | Debug overlay | 1.92.8, features `docking-experimental`, `sdl3-binding`, `sdlgpu3-binding` | MIT | Yes | Private to `tools` | M3 |
| EnTT | Scene entity storage | 3.16.0 | MIT | Yes | Permitted in `atlas/scene` headers by ADR-0004; in practice private to `scene/src` | M5 |
| nlohmann-json | Reading and writing the scene file, and reading animation clips and string tables | 3.12.0, port-version 2 | MIT | Yes | Private to `scene/src` and `assets/src`; no JSON type appears in any Atlas header, and `atlas::text` receives plain strings rather than linking this | M5, second consumer M13, third call site M16 |
| WAMR | The WebAssembly interpreter sandboxed mods run in | WAMR-2.4.5, pinned by REF and SHA512 in `external/vcpkg-overlays/wasm-micro-runtime/portfile.cmake`. **The only overlay port in this project**, because no WebAssembly runtime exists in vcpkg — not at our baseline and not upstream, checked 2026-09-18 | Apache-2.0 WITH LLVM-exception | Yes, one way, the same footing as SPIRV-Cross | Private to `script/src`; no WebAssembly type appears in any Atlas header, which is what keeps ADR-0015's Luau fallback real | M15 |

Dependencies are added in the milestone that first needs them, never in advance.

Versions are whatever the pinned baseline provides, not whatever is newest upstream.
Catch2 is 3.15.3 rather than the 3.16.0 on vcpkg's master branch for exactly that reason:
the baseline is the thing that makes the build reproducible, so the baseline decides.

Host prerequisites, beyond a compiler and CMake: `ninja`, `pkg-config` (vcpkg's Catch2 port
needs it), and Git. On macOS, `brew install ninja pkgconf`.

## Notes and constraints

**Tracy version lock.** The Tracy client and the profiler GUI must match: the network
protocol changes between releases. The pinned client is 0.13.1, which matches the Homebrew
`tracy` formula. Upstream 0.14.x uses a different protocol version and must not be mixed.
Upgrade both together or neither.

**SDL3 version.** 3.4.12 is what the pinned baseline provides; 3.4.16 exists upstream and
is not used, for the same reason Catch2 is 3.15.3. The `vulkan` feature is deliberately off
until M2, which is when the GPU backend that needs it arrives.

**SDL3 build prerequisites on Linux.** Building SDL3 from source needs the X11, Wayland,
xkbcommon, EGL, ALSA, PulseAudio, udev, D-Bus and ibus development packages, even for a
headless build, because the library still compiles those backends. `tools/ci/linux_deps.sh`
installs them.

**SDL3 on macOS.** SDL_GPU uses Metal natively. MoltenVK and the Vulkan SDK are not
required. The Vulkan feature of the SDL3 port is enabled on Linux only.

**SDL3 must come from vcpkg.** A Homebrew `sdl3` may exist on the development machine.
`cmake/Dependencies.cmake` asserts that the resolved SDL3 lives under `vcpkg_installed`, so
a stale system copy cannot be picked up silently.

**SDL_shadercross is not used.** Its vcpkg port depends on `directx-dxc`, which supports
Windows and Linux x86_64 only, so it cannot be installed on the development machine. glslang
and SPIRV-Cross replace it and are in the pinned baseline for every platform. The cost is
DXIL, and with it the Direct3D 12 backend; see ADR-0006. Cooked shader outputs are committed,
so no contributor or continuous-integration job needs the toolchain to build and run Atlas.
Changing a shader needs it: `brew install glslang spirv-cross`, or the apt equivalents.

**No Metal shader compiler.** The development machine has Command Line Tools but not full
Xcode, so there is no `metal` compiler. Atlas ships Metal shaders as MSL source text, which
SDL_GPU compiles at device creation. Precompiled `.metallib` is not produced.

**EnTT version.** Upstream 4.0.0 requires C++20 and changes the API. vcpkg ships 3.16.0.
Atlas pins 3.16.0 and confines all EnTT usage to the `scene` wrapper, so a later move is
contained.

**stb_image.** Compiled once, inside the assets module, and its include directory is marked
SYSTEM so that its warnings are not held to Atlas's first-party standard: it is C from
another project and will not stop using old-style casts because this build asked it to. The
decoder does not bound its own allocations, so image dimensions are checked against explicit
limits before anything is decoded; an image header is untrusted input and a header claiming
enormous dimensions is the standard way to turn a decode into an allocation failure.

**Dear ImGui.** Version 1.92.8 is what the pinned baseline provides. Only the SDL_GPU
renderer backend is used; the SDL3 platform backend is not, because it would require raw
window-system events inside `atlas::tools`, and Atlas drives the overlay's input from its
own event types instead.

`ImGui_ImplSDLGPU3_PrepareDrawData` uploads vertex data through a copy pass, which the
graphics library refuses to nest inside a render pass. `DebugUi` therefore splits preparing
from drawing, and drawing requires a token that only preparing can produce, so the wrong
order fails to compile rather than aborting at run time.

## Replacement boundaries

| Library | Could be replaced by | Cost |
|---|---|---|
| SDL3 / SDL_GPU | A native Vulkan, D3D12, or Metal backend behind `atlas::rhi` | Reimplementing the RHI back end; no public API change if the boundary held |
| EnTT | Another ECS, or first-party storage | Rewriting the `scene` wrapper only |
| Dear ImGui | Another immediate-mode UI | Rewriting `tools` panels |
| Tracy | Another profiler | Remapping the macros in `core/profile.hpp` |
| Catch2 | Another test framework | Mechanical test rewrite |
| stb_image | libpng plus libjpeg-turbo | Rewriting one importer |
| nlohmann-json | Another JSON library, or a bespoke format | Rewriting `scene/src/serialization.cpp`, `assets/src/animation_clip.cpp` and `assets/src/string_table.cpp`; no caller changes. The last of those also uses the SAX interface, which not every library has an equivalent of |

## Update policy

1. Dependencies are updated deliberately, never as a side effect of moving the baseline.
2. An update changes this file first: version, date, and the reason.
3. CI must be green on all three platforms before the update is committed.
4. Tracy is updated only in lockstep with the profiler GUI.
5. A dependency with an incompatible licence is not adoptable, whatever its merits.

## Considered and not adopted

glm, fmt, spdlog, and xxhash are all available and are deliberately not used: logging and
formatting are served by `std::format`, hashing by a first-party canonical hash whose
algorithm identity is versioned, and math by a small first-party header. Each would be
adopted only with a recorded need.

**miniaudio**, **SDL3_mixer** and **dr_libs**, considered in M12 and not adopted. Each would
have supplied decoding and mixing together. What Atlas needed was the parts it had to decide for
itself anyway — the threading model, the error model, the handle model, and where the boundary
with `assets` falls — and what remained after those decisions was a mixer of about a hundred
lines of arithmetic. SDL3_mixer would also have meant a second library owning audio device
lifetime alongside the platform, which [ADR-0011](adr/0011-audio.md) rejected on its own terms.

**lua**, **luau** and **sol2**, considered twice and not adopted
([ADR-0009](adr/0009-scripting-decision.md) in M9, [ADR-0015](adr/0015-sandboxed-mods.md) in
M15). In M9 the objection was that there was no caller. In M15 there is one, and the objection
is determinism: `docs/DETERMINISM.md` forbids libm transcendentals in authoritative code, and a
mod's decisions are authoritative input under lockstep, so `math.sin` and `^` are one call away
from a divergence in both languages. The only defence is removing library functions, which is
exactly the part a demonstration script never exercises.

Both are cheaper than what was chosen and the record says so: stock ports, MIT, all platforms,
no transitive dependencies, nothing to maintain. **Luau is ADR-0015's named fallback with a
written trigger** — if the WAMR overlay port cannot be built on all three platforms — rather
than a rejected alternative, and it is the strongest of the three on sandboxing. Lua 5.5 also
retired ADR-0009's specific objection to it: `lua_newstate` now takes the string-hash seed as an
argument, so the overlay port that objection required is no longer needed.

nlohmann-json was on this list until M5. The recorded need is the scene file format
([ADR-0007](adr/0007-scene-file-format.md)): the file is untrusted input, and a hand-written
parser for untrusted input is the kind of code this project should not be writing when a
hardened one is a dependency away. It is used through `ordered_json`, whose key order is the
writer's rather than a hash's, and through the non-throwing parse overload, because
exceptions do not cross module boundaries here.
