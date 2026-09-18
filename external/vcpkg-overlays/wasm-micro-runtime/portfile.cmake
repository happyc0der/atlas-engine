# SPDX-License-Identifier: GPL-3.0-or-later
#
# WebAssembly Micro Runtime, interpreter only.
#
# An overlay port because there is no WebAssembly runtime in vcpkg: not at the baseline this
# project pins, and not upstream either, checked on 2026-09-18. ADR-0015 records that, records
# that owning this file is the cost of the decision, and names the fallback if it cannot be made
# to build on all three platforms.
#
# The pin is this file. An overlay port takes no `overrides` entry in the manifest, because it
# is not in the versions database — the REF and the SHA512 below are what fix the bytes, which
# is a stronger pin than a version string and is the reason the port is committed rather than
# fetched.
#
# **Every option below is a decision, not a default.** WAMR's own defaults enable AOT
# compilation, WASI, SIMD and a builtin libc; each is either a determinism hazard or an
# authority a sandboxed mod must not have, so each is turned off explicitly and with a reason.
# A future version that adds a feature will default it on, and that is exactly what this list
# exists to prevent.

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO wasm-micro-runtime/wasm-micro-runtime
    REF "WAMR-${VERSION}"
    SHA512 3aadee3befdd9a8f4fb45c13800e98145ef5492843b08715d9d6787dc9261fb345cc9005d9544efb184f53c83dfe495c176d97b0f05c729db76069f3e3aea60e
    HEAD_REF main
    PATCHES
        # Two MSVC problems with the same cause: upstream builds Windows through
        # product-mini/platforms/windows and as a DLL, so the top-level CMakeLists this port
        # uses has never been asked to produce a static library for MSVC.
        #
        #   1. `-lm -ldl` are linked unconditionally and PUBLIC, so both reach every consumer
        #      through the exported `iwasm::vmlib` target. MSVC's linker understands neither.
        #      Guarded rather than deleted: everywhere else they are correct and needed.
        #   2. The public header defaults MSVC consumers to `__declspec(dllimport)`, which asks
        #      the linker for `__imp_` symbols a static library does not have — sixteen
        #      unresolved externals, and only on Windows. The header guards the macro with
        #      `#ifndef` precisely so a static build can say otherwise, which is what the
        #      INTERFACE definition does.
        #
        # Drop this patch if upstream ever supports a static MSVC build from here, rather than
        # carrying it for ever.
        msvc-static-library-fixes.patch
)

# Debug and release are configured one after the other rather than together.
#
# WAMR's `build-scripts/version.cmake` calls `configure_file` to write `core/version.h` into the
# **source** tree rather than the build tree, and vcpkg configures both variants in parallel out
# of one extracted source. The two writes race for the same path, and the loser fails with
# "No such file or directory" from `configure_file`. It is timing-dependent, which is why it
# passed on this machine and on Linux and failed on the macOS runner — the worst kind of
# problem to leave in a port that every lane of continuous integration has to build.
#
# This is the documented escape hatch for exactly this shape, and around 135 ports in the vcpkg
# registry use it for the same reason. It costs a little configure time and buys a port that
# builds the same way every time.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    DISABLE_PARALLEL_CONFIGURE
    OPTIONS
        # The interpreter, and nothing that generates code. An ahead-of-time or just-in-time
        # compiler would make what a guest computes depend on which compiler ran, which is the
        # one property mods must not have: every peer must reach the same answer.
        -DWAMR_BUILD_INTERP=1
        -DWAMR_BUILD_AOT=0
        -DWAMR_BUILD_JIT=0
        -DWAMR_BUILD_FAST_JIT=0

        # The classic interpreter rather than the fast one, and instruction metering on. These
        # two are one decision: WAMR only meters the classic interpreter, and a budget counted
        # in instructions is what ADR-0015 D7 requires — a wall-clock watchdog fires at
        # different points on different machines, which under lockstep is divergence dressed up
        # as safety. Metering defaults to **off**, so leaving this out would have removed the
        # budget silently rather than loudly.
        -DWAMR_BUILD_FAST_INTERP=0
        -DWAMR_BUILD_INSTRUCTION_METERING=1

        # No ambient authority. WASI is a filesystem, a clock and an environment; a builtin
        # libc is a second set of imports nobody declared. A mod gets the imports the host
        # hands it and nothing else, which is what ADR-0015 means by "no ambient authority".
        -DWAMR_BUILD_LIBC_WASI=0
        -DWAMR_BUILD_LIBC_BUILTIN=0
        -DWAMR_BUILD_LIB_PTHREAD=0
        -DWAMR_BUILD_LIB_WASI_THREADS=0
        -DWAMR_BUILD_MULTI_MODULE=0

        # SIMD off. Relaxed SIMD is non-deterministic by specification — the same instruction
        # is permitted to give different answers on different hardware — and the cost of
        # keeping only the deterministic half would be trusting a build flag to stay honest
        # across upgrades. There is no measured need for it in a mod.
        -DWAMR_BUILD_SIMD=0

        # No shared memory and no threads inside the sandbox: a guest that can race is a guest
        # whose result depends on scheduling.
        -DWAMR_BUILD_SHARED_MEMORY=0

        # Bulk memory and reference types are deterministic and are what current toolchains
        # emit by default; refusing them would refuse ordinary modules for no gain.
        -DWAMR_BUILD_BULK_MEMORY=1
        -DWAMR_BUILD_REF_TYPES=1

        # No compiled-module cache: it would write files, and its correctness across versions
        # is one more thing to trust.
        -DWAMR_BUILD_WASM_CACHE=0

        -DBUILD_SHARED_LIBS=OFF
)

vcpkg_cmake_install()

# WAMR installs a package config that exports `iwasm::vmlib`.
vcpkg_cmake_config_fixup(PACKAGE_NAME iwasm CONFIG_PATH lib/cmake/iwasm)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
