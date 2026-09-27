<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# 0025 — Windows renders on Vulkan, and a real GPU says so

## Status

**Accepted**, 2026-09-27, at the end of M28. Proposed 2026-09-26 at the milestone's gate, before
anything was installed on the laptop or changed in the build. What happened differs from what was
decided in three places, each noted where it is decided below and scored under
[What M28 found](#what-m28-found).

Makes true what [ADR-0006](0006-shader-toolchain.md) and the roadmap already say — *"Windows uses
the Vulkan backend"* — which the build does not do. Records ADR-0006's trigger for Direct3D 12 as
fired and defers it to its own milestone. Supersedes nothing.

## Context

The roadmap's largest recorded risk is that *"no real non-Apple hardware has ever run this
code"*, with *"No local Windows machine"* beside it. Windows is compiled and tested on every push,
but hosted runners have no GPU, so the `gpu` label is excluded there, and the renderer has run on
one Apple GPU and on llvmpipe and nowhere else. The owner has a Windows laptop reachable over SSH,
`msi`, with an NVIDIA RTX 3080 Ti Laptop GPU, and on 2026-09-26 chose this milestone.

### What the survey found

**Windows very likely cannot render today, and nothing in CI can notice.** This is inferred from
the code and from SDL's source, not observed; the first slice observes it before anything is
changed.

- `engine/rhi/src/device.cpp` asks SDL for SPIR-V, MSL **and DXIL** on every platform, under a
  comment saying it asks "for every format Atlas can supply". Atlas ships no DXIL.
- SDL 3.4's backend order is Metal, then Direct3D 12, then Vulkan. With DXIL requested, Direct3D
  12 qualifies on Windows and wins. `renderer/shader_loader` then finds neither SPIR-V nor MSL
  acceptable, and every shader load fails.
- `vcpkg.json` enables SDL's `vulkan` feature **only on Linux**. On Windows SDL is built without
  it, and its GPU Vulkan backend depends on it, so Windows' SDL probably has no Vulkan backend at
  all: even forcing `SDL_GPU_DRIVER=vulkan` would fail.
- `DeviceDesc::preferred_backend` exists and no caller sets it.
- A device that comes up but cannot load shaders **fails** the GPU tests rather than skipping
  them. No lane runs them on Windows, so the failure has never been seen.

**The laptop.** Windows 11 Home, NVIDIA driver 616.92, Vulkan 1.4. Over SSH it runs in session 0,
which is not interactive, and `vulkaninfo` there still sees exactly one device, the RTX 3080 Ti.
The owner is logged in at the desktop in session 1. It has Visual Studio 2019 Build Tools only,
which cannot build Atlas, and no Vulkan SDK, so no validation layer. Third-party Vulkan layers
are installed (an overlay and a capture hook) and may print warnings.

### What the laptop found, which the survey could not

*Added during M28, 2026-09-27.*
- **The inference was right.** Unchanged, the device came up as `direct3d12`, accepting
  `dxil=true` and neither SPIR-V nor MSL, and every GPU test that loads a shader failed. SDL's own
  Windows configure log reads `SDL_VULKAN … OFF` and `GPU drivers: d3d12`.
- **From SSH, the GPU tests skip rather than fail.** Session 0 cannot create a swapchain, and
  Atlas creates its device together with the window's swapchain, so every case reported "no
  graphics device" and CTest printed "98% tests passed" for a machine that drew nothing. The
  Windows GPU run happens in the desktop session, and must fail on a skipped GPU test.
- **`Window::display_scale()` broke its own contract on Windows.** It promises the ratio of
  pixels to logical units, which every caller uses to turn a pointer's position into a pixel.
  But it returned SDL's display scale, which is that ratio times the user's interface scaling.
  On a Retina Mac the two are equal. On this laptop, set to 175%, pixels equal logical units and
  it returned 1.75, so a click in the lab picked a cell 1.75 times too far from the pointer. It
  now returns the pixel density. The name stays, because it is installed API; its comment says
  what it means. A new GPU case fails under the old code on this laptop, and passes on the Mac
  and on llvmpipe either way.
- **The lab's live pick check compared the identifier pass with the analytic inverse at
  different points**: the pixel's centre against its corner. It now uses the centre, as the GPU
  test of the same pass always did.
- **A device test counted DXIL as a format Atlas could produce**, so a device that could draw
  nothing of Atlas's passed it. It now counts only the shipped formats.

### The owner's decisions, 2026-09-26

- **Vulkan now, Direct3D 12 its own later milestone.**
- **Install the correct tools and remove the old one**: Visual Studio 2026 Build Tools, the
  toolset CI's Windows runner compiles with (MSVC 19.51), in place of 2019.
  *Changed by the owner, 2026-09-27:* **2019 stays.** The laptop's CUDA 12.1 accepts only
  Visual Studio 2017 to 2022 17.9 as `nvcc`'s host compiler, and 2026 is newer, so removing 2019
  would have left CUDA with no compiler it accepts. Atlas builds with 2026 from a developer shell
  that names it; the two do not meet.
- **A script run over SSH** makes the Windows GPU run repeatable. No self-hosted runner, because
  the repository is public.
- **The owner's desktop session may be used** for what needs a visible window, if session 0
  cannot make one.

### Predictions, written before anything is built

Each is written against the state it will meet, M27's lesson: none forecasts a failure an
earlier slice of this plan fixes first.

1. On the laptop, before any change, the Debug build passes every test CI runs (1013), and
   every `gpu` test that loads a shader **fails** rather than skips. The log shows Direct3D 12
   chosen, or no device.
2. The new backend test fails on Windows before D1 and D2 and passes after. It passes on macOS
   and Linux throughout.
3. After D1 and D2, with no renderer change, every `gpu` test passes on the RTX 3080 Ti.
4. The identifier pass agrees exactly, with zero pick disagreements, because it is integers.
5. From session 0, a Vulkan device and offscreen targets work, and a swapchain does not.
6. Vulkan validation reports at least one warning in Atlas's own use of SDL_GPU. Nothing has
   run under validation on real Vulkan hardware before.
7. `renderer/quad_batch_submit` and `lab/cell_field_submit` on the i9-12900HX land within ±50%
   of the M4 Pro's CPU-side medians. They measure the CPU, not the GPU.
8. Rebuilding SDL with Vulkan adds under five minutes to a cold Windows CI dependency build.

## Decision

**D1. SDL is built with Vulkan on Windows.** The `sdl3` entry with the `vulkan` feature covers
Windows as well as Linux. That changes the Windows SDL binary's ABI hash, so CI rebuilds SDL on
Windows once.

**D2. The device asks only for shader formats Atlas ships.**
- One list, beside the shader loader, names the formats Atlas's committed shaders come in:
  SPIR-V and MSL. The device requests exactly those.
- With DXIL gone from the request, SDL cannot choose a backend Atlas has no shaders for.
  Direct3D 12 is ineligible, and Windows gets Vulkan by SDL's own order, with no preference to
  set or maintain.
- `preferred_backend` stays, unset, for a person or test that needs to force one.

**D3. A test that needs no GPU catches this class of mistake in hosted CI.**
- It asks SDL which GPU backends were compiled into this build and which shader formats each
  takes, and requires at least one backend that can load a format Atlas ships.
- On today's Windows build it would fail. It carries the `unit` label and needs no device, so
  it runs in every lane, the sanitizers included.

**D4. Windows GPU verification is a script over SSH.**
- `tools/ci/windows_gpu.ps1` builds a GPU preset and runs: the `gpu` label; the pick case; a
  sandbox and a lab screenshot; and the GPU benchmark groups. It prints one summary and hands the
  screenshots back.
- What needs a window runs in the owner's desktop session, through a scheduled task, **only if**
  the first slice shows session 0 cannot do it. The task is run once, waited on and deleted.
  *2026-09-27:* on Vulkan session 0 **can** present, and every GPU test passes there. The
  script runs them in the desktop session anyway, for a reason this record did not foresee:
  session 0's display has no scaling, so the display-scale case slice 2 added passes there under
  the broken code too.
- New presets `windows-msvc-debug-gpu` and `windows-msvc-release-gpu` mirror `macos-debug-gpu`.
- Each report that touches rendering states the script's result.
- *2026-09-27:* built as `tools/ci/windows_gpu.sh`, run from the development machine, which
  checks a pushed commit out on the laptop and runs `tools/ci/windows_gpu.ps1` there. It fails on
  a skipped test as well as a failed one, reading CTest's JUnit report, because CTest's summary
  counts a skip as a pass. The pick case is in the `gpu` label, so it is not run separately.

**D5. Vulkan validation is on in the Windows GPU presets.** The Khronos validation layer is
installed on the laptop, as Metal's validation already runs in the Mac preset. A validation error
is a finding: fixed, or recorded with its reason if it is SDL's.

**D6. Direct3D 12 is its own milestone, and ADR-0006's trigger is recorded as fired.** It needs:
- DXC or shadercross on Windows;
- DXIL among the committed shader outputs, with a check that compares only what this repository
  decides;
- a third format in the loader;
- the GPU tests on both backends.

**D7. What stays unverified is named**:
- AMD and Intel GPUs;
- Windows on arm64;
- Direct3D 12;
- device loss on real hardware, which cannot be caused on purpose;
- GPU time, since SDL_GPU has no timestamp queries.

## Alternatives

**Prefer Vulkan by name on Windows and keep requesting DXIL.** It would work while the preference
was set. But it would keep a request for a format that does not exist, and a new caller that
forgot the preference would be back on Direct3D 12. Requesting only what exists makes the wrong
backend unrepresentable.

**Direct3D 12 now.** It is the native backend and ADR-0006's trigger has fired. Rejected by the
owner for this milestone: it roughly doubles the work, and it adds a committed output whose bytes
come from a compiler that exists only on Windows. That is the M13 trap, and it needs its own
record.

**A self-hosted GitHub runner on the laptop.** It would put the GPU run in CI. Rejected: on a
public repository, a pull request from anyone could run code on the owner's laptop unless the
runner were locked down carefully, and the laptop sleeps on battery anyway.

**Run everything from SSH and accept what session 0 cannot do.** Kept as the first thing tried.
It is the fallback only if session 0 cannot present, and the owner allowed the desktop session.

## Consequences

- `vcpkg.json` and `engine/rhi/src/device.cpp` change; the Windows CI cache rebuilds SDL once.
- A new `unit` test in `engine/rhi/tests`, two new presets, and `tools/ci/windows_gpu.ps1`.
- On the laptop: Visual Studio 2026 Build Tools and the Vulkan SDK installed, and a clone at
  `C:\src\atlas-engine`. *2026-09-27:* Visual Studio 2019 stays, by the owner's decision above.
- ROADMAP, ARCHITECTURE and DEFERRED stop saying Windows uses Vulkan as a fact and start saying
  how that is known.

## What M28 found

*Added 2026-09-27.* The full account is [reports/M28.md](../reports/M28.md).

| # | Prediction | Outcome |
|---|---|---|
| 1 | Unchanged, the laptop passes CI's set and every shader-loading `gpu` test fails | **Held.** 1013 of 1013; in the desktop session the device was `direct3d12` and 15 shader-loading tests failed. |
| 2 | The backend test fails on Windows before D1 and D2 and passes after | **Held.** It named the cause: "the library was built with: direct3d12". |
| 3 | After D1 and D2, with no renderer change, every `gpu` test passes | **Missed.** Two fixes outside the renderer were needed: `display_scale` returned the interface scaling, and the lab's live pick check compared a pixel's centre with its corner. |
| 4 | Zero pick disagreements | **Missed**, once, and it was the display-scale bug seen end to end: 3062 against 3061. None after the fix. |
| 5 | From session 0 a device works and a swapchain does not | **Missed.** On Vulkan session 0 presents; it was Direct3D 12 that could not. |
| 6 | Validation reports at least one warning in Atlas's use of SDL_GPU | **Missed.** None, with the layer loaded and its messages reaching the log. |
| 7 | The GPU benchmarks land within ±50% of the M4 Pro | **Missed** on every row, by 2.6 to 15 times. A plain loop with no engine streams memory thirty times slower on the laptop than on the M4 Pro, and the large rows follow it (PERFORMANCE.md). |
| 8 | Rebuilding SDL with Vulkan adds under five minutes to a cold Windows CI build | **Held.** 57 s in Release and 1.2 min in Debug. |

**Three held and five missed.** The misses about the renderer were in its favour: it needed no
change, and validation found nothing. The misses about everything around it were the milestone's
findings. The two fixes lived in the platform and the lab, where no GPU test on the Mac could
reach, because on a Retina display a pixel density and a display scale are the same number.

## Rollback cost

**Low.** The feature line, the format list and a test. Reverting them returns Windows to where
it is today, which is not rendering.
