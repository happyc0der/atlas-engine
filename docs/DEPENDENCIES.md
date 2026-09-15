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
| SDL_shadercross | Offline HLSL to SPIR-V, DXIL, MSL, and reflection | commit pin; no upstream release exists | zlib | Yes | Build-time tool only; never linked into engine targets | M2 |
| stb | Image decoding (`stb_image`) | 2024-07-29 | MIT / Unlicense | Yes | Private to the assets importer | M3 |
| Dear ImGui | Editor and debug UI | 1.92.9, docking | MIT | Yes | Private to `tools` | M3 |
| EnTT | Scene entity storage | 3.16.0 | MIT | Yes | Permitted in `atlas/scene` headers by ADR-0004 | M5 |

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

**SDL_shadercross has no releases.** It is pinned by commit. The vcpkg port depends on
`directx-dxc`, which does not support `arm64-osx`, so the macOS build is from source with
vendored dependencies, following the project's own macOS CI recipe. Slang is the documented
fallback front end. Cooked shader outputs are tracked in the repository, so no contributor
or CI job needs the toolchain to build and run Atlas.

**No Metal shader compiler.** The development machine has Command Line Tools but not full
Xcode, so there is no `metal` compiler. Atlas ships Metal shaders as MSL source text, which
SDL_GPU compiles at device creation. Precompiled `.metallib` is not produced.

**EnTT version.** Upstream 4.0.0 requires C++20 and changes the API. vcpkg ships 3.16.0.
Atlas pins 3.16.0 and confines all EnTT usage to the `scene` wrapper, so a later move is
contained.

**Dear ImGui features.** The vcpkg feature names are `docking-experimental`,
`sdl3-binding`, and `sdlgpu3-binding`. The SDL_GPU backend requires
`ImGui_ImplSDLGPU3_PrepareDrawData` to be called before the render pass that draws the UI;
this ordering is enforced inside the renderer's frame structure rather than in application
code.

## Replacement boundaries

| Library | Could be replaced by | Cost |
|---|---|---|
| SDL3 / SDL_GPU | A native Vulkan, D3D12, or Metal backend behind `atlas::rhi` | Reimplementing the RHI back end; no public API change if the boundary held |
| EnTT | Another ECS, or first-party storage | Rewriting the `scene` wrapper only |
| Dear ImGui | Another immediate-mode UI | Rewriting `tools` panels |
| Tracy | Another profiler | Remapping the macros in `core/profile.hpp` |
| Catch2 | Another test framework | Mechanical test rewrite |
| stb_image | libpng plus libjpeg-turbo | Rewriting one importer |

## Update policy

1. Dependencies are updated deliberately, never as a side effect of moving the baseline.
2. An update changes this file first: version, date, and the reason.
3. CI must be green on all three platforms before the update is committed.
4. Tracy is updated only in lockstep with the profiler GUI.
5. A dependency with an incompatible licence is not adoptable, whatever its merits.

## Considered and not adopted

glm, fmt, spdlog, nlohmann-json, and xxhash are all available and are deliberately not
used yet: logging and formatting are served by `std::format`, hashing by a first-party
canonical hash whose algorithm identity is versioned, and math by a small first-party
header when M3 needs it. Each would be adopted only with a recorded need.
