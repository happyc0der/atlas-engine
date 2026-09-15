# 0001 — Language, build system, and target platforms

## Status

Accepted, 2026-09-14.

## Context

Atlas is a long-lived engine platform for a data-heavy world simulation. It needs explicit
control over memory layout and lifetime, a mature native ecosystem for graphics and
platform libraries, and reproducible builds on more than one operating system. It is
developed by one person on a single machine: an Apple M4 Pro running macOS 26.5 with Apple
clang 21, Command Line Tools rather than full Xcode, and no local Windows or x86_64
hardware.

## Decision

**C++23.** Verified available on the development machine: `std::expected`, `std::format`,
`std::span`, `std::source_location`, `std::jthread`, `std::flat_map`, and `std::mdspan` all
compile with Apple clang 21 and libc++. `std::expected` in particular is the basis of the
error model in ADR-0005.

**CMake with Ninja and checked-in presets.** `CMakePresets.json` is tracked and defines
every supported configuration; `CMakeUserPresets.json` is untracked and holds local
overrides such as alternate compiler or tool paths.

Presets use **schema version 8** and the project requires **CMake 3.28**. CMake 4.4 supports
schema 12, but the floor is set by the oldest environment Atlas must build in, and that is
a plain Ubuntu 24.04 image, whose apt CMake is 3.28.3 (verified in a container, not assumed
from the GitHub runner image, which carries a newer 3.31.6). Schema 8 covers every preset
feature Atlas uses, including workflow presets, which need only schema 6. Requiring a newer
CMake would buy nothing and would force an extra install step on Linux.

**vcpkg in manifest mode, as a git submodule at `external/vcpkg`,** pinned to the commit
that release 2026.07.29 resolves to, with every direct dependency additionally pinned by
`overrides`. A submodule rather than a `VCPKG_ROOT` environment variable means a fresh
clone plus bootstrap reproduces the dependency set with no machine configuration.

**Tier-one platforms: macOS arm64, Linux x86_64, Windows x64.** The specification named
only Windows and Linux; macOS is added because it is the only machine the work actually
happens on, and an untested development platform is a fiction. The tiers are honest about
what is verified:

| Platform | Compiler | Verification |
|---|---|---|
| macOS 15+ arm64 | Apple clang 21 | Developed and GPU-verified on real hardware |
| Linux x86_64 | Clang 19+ | Headless in CI, plus a software-Vulkan GPU lane |
| Windows x64 | MSVC 19.4x | Headless in CI; GPU path unverified |

GCC is expected to work and is not a CI gate.

**Clang 19 is the Linux floor, and the reason is specific.** `std::expected` is the basis of
the error model in ADR-0005, and on Linux it turned out not to be available with the
compiler Ubuntu 24.04 ships by default. The cause is not the standard library version:
libstdc++ gates `<expected>` on `__cpp_concepts >= 202002L`, and Clang 18 reports
`201907`, so Clang 18 cannot see `std::expected` with *any* libstdc++. Clang 19 reports
`202002` and works. Both Clang 19 and libstdc++ 14 are in Ubuntu 24.04's own repositories,
so no third-party apt source is needed.

libstdc++ rather than libc++: Clang 18 with libc++ would also work, but vcpkg builds
dependencies with GCC and libstdc++, and mixing standard library implementations across
that boundary invites ABI problems for no benefit. GCC 14 also works and remains the
best-effort secondary compiler.

**MSVC and C++23.** MSVC has no `/std:c++23` switch. `/std:c++23preview` exists but is
explicitly not ABI-stable across releases, which is unacceptable with a binary dependency
cache. CMake's `CXX_STANDARD 23` therefore maps to `/std:c++latest` on MSVC. The C++23
library features Atlas relies on are available there: `std::expected` since VS 2022 17.3 and
`std::format` and `std::jthread` earlier. `std::print` is deliberately not used in engine
code, because the Ubuntu 24.04 CI image's default libstdc++ 13 lacks `<print>`.

**Determinism-relevant flags from the start:** `-ffp-contract=off` on Clang and GCC,
`/fp:precise` on MSVC, no fast-math anywhere, and `long double` forbidden. These cost
nothing now and are expensive to retrofit once results depend on them.

## Alternatives

**Rust** has a stronger safety story and a good build system, but a weaker ecosystem for
the specific libraries Atlas depends on, and the owner's stated goal includes learning
systems programming in C++. Rejected.

**C++20** would avoid the MSVC `/std:c++latest` question, but `std::expected` is the single
feature the error model is built on, and reimplementing it is pure cost. Rejected.

**Meson or Bazel** are defensible build systems. CMake wins on library ecosystem: nearly
every dependency ships CMake config packages, and vcpkg integrates through a CMake
toolchain file. Rejected.

**Conan** is a reasonable alternative to vcpkg. vcpkg's manifest mode with a pinned
baseline and a submodule gives exactly the reproducibility wanted, and its CMake toolchain
integration is a single cache variable. Rejected, without prejudice.

**CMake FetchContent** needs no extra tool, but has weaker binary caching and reconfigures
slowly. Rejected; it remains the fallback if vcpkg becomes an obstacle.

**Windows and Linux only,** as the specification proposed, would leave the only machine the
project is built on outside the supported set. Rejected.

## Consequences

- Three CI legs and three sets of platform-specific breakage to fix, without local access
  to two of them. MSVC-only and Linux-only failures are found in CI, not on the desk. Local
  `-Wconversion` and `-Wsign-conversion` act as a proxy for MSVC's most common warnings, and
  a Docker container gives Linux toolchain parity, though on arm64 rather than x86_64.
- Presets schema 10 forgoes newer preset features until CI images carry CMake 4.4.
- MSVC builds use `/std:c++latest`, which tracks a moving target; a C++26 feature could be
  accepted by MSVC and rejected elsewhere. Warnings-as-errors and CI catch this.
- The first configure on each machine builds dependencies from source. A binary cache
  directory, cached in CI, keeps this to a one-time cost.
- Windows GPU behaviour stays unverified until hardware exists. This is stated in the
  charter rather than papered over.

## Rollback cost

Changing the build system or dependency manager is a contained, mechanical change: presets,
the `cmake/` helpers, and the manifest. Days, not weeks. Changing the language is not a
rollback, it is a rewrite. Dropping a platform tier is trivial; adding one later means
fixing however much breakage accumulated while it was unwatched.
