#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Install Atlas from a built preset, and say how another project builds against it (ADR-0024).

The SDK is two directories and a contract (D9). One is the install prefix `cmake --install`
writes: the engine's headers, its static libraries, its CMake package and its runtime data. The
other is the vcpkg tree the build resolved its third-party libraries from, used where it is and
never copied, so a consumer links exactly the libraries Atlas was compiled against. A consumer
puts both on CMAKE_PREFIX_PATH, installs no vcpkg of its own, and uses the same compiler and major
version, the same triplet and, with MSVC, a build type of the same kind. AtlasConfig.cmake checks
the parts of that it can.

Usage:
    python3 tools/sdk.py macos-debug                    # install to the preset's installDir
    python3 tools/sdk.py macos-debug --prefix /tmp/sdk  # or somewhere else
    python3 tools/sdk.py linux-clang-debug >> "$GITHUB_ENV"
    python3 tools/sdk.py --build-dir build/macos-debug --prefix /tmp/sdk   # a build by path

Standard output is two lines a shell or a CI environment file can take as they are:

    ATLAS_PREFIX=<the install prefix>
    ATLAS_DEPS=<the vcpkg tree>

Everything else — what was built, and what a consumer must match — goes to standard error. The
preset must already be configured and built. A profile or sanitizer build refuses to install (D7),
and so does this.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def read_cache(build_dir: Path) -> dict[str, str]:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        raise SystemExit(f"error: {build_dir} is not configured; run `cmake --preset "
                         f"{build_dir.name}` and build it first")
    values: dict[str, str] = {}
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"^([A-Za-z_][A-Za-z0-9_]*):[A-Z_]+=(.*)$", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def recorded(config: Path, name: str) -> str:
    """A value AtlasConfig.cmake recorded about the build, such as the compiler that made it."""
    match = re.search(rf'set\({name} "([^"]*)"\)', config.read_text(encoding="utf-8"))
    return match.group(1) if match else ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("preset", nargs="?",
                        help="a configured and built preset, such as macos-debug")
    parser.add_argument("--build-dir", help="a configured and built build directory, instead of "
                                            "a preset's")
    parser.add_argument("--prefix", help="install here instead of the preset's installDir")
    args = parser.parse_args()
    if bool(args.preset) == bool(args.build_dir):
        parser.error("name a preset or a --build-dir, and not both")

    build_dir = Path(args.build_dir).resolve() if args.build_dir else ROOT / "build" / args.preset
    cache = read_cache(build_dir)

    command = ["cmake", "--install", str(build_dir)]
    if args.prefix:
        command += ["--prefix", args.prefix]
    # The install's own listing would bury the two lines this prints, so it goes to stderr.
    result = subprocess.run(command, stdout=sys.stderr, check=False)
    if result.returncode != 0:
        print(f"error: `{' '.join(command)}` failed; see above", file=sys.stderr)
        return result.returncode

    prefix = Path(args.prefix or cache["CMAKE_INSTALL_PREFIX"]).resolve()
    triplet = cache.get("VCPKG_TARGET_TRIPLET", "")
    deps = Path(cache.get("VCPKG_INSTALLED_DIR", "")) / triplet
    if not triplet or not (deps / "share").is_dir():
        print(f"error: no vcpkg tree at {deps}; was this preset configured with the vcpkg "
              f"toolchain?", file=sys.stderr)
        return 1

    config = prefix / "lib" / "cmake" / "Atlas" / "AtlasConfig.cmake"
    compiler = " ".join(filter(None, [recorded(config, "Atlas_BUILT_WITH_COMPILER_ID"),
                                      recorded(config, "Atlas_BUILT_WITH_COMPILER_VERSION")]))
    build_type = recorded(config, "Atlas_BUILD_TYPE")
    deployment = recorded(config, "Atlas_OSX_DEPLOYMENT_TARGET")

    print(f"ATLAS_PREFIX={prefix}")
    print(f"ATLAS_DEPS={deps}")

    notes = [
        "",
        f"Atlas installed to {prefix}",
        f"  built by   {compiler}, {build_type}, triplet {triplet}",
        f"  libraries  {deps}",
        "",
        "Build against it with:",
        f'  -DCMAKE_PREFIX_PATH="{prefix};{deps}"',
        "and, in CMakeLists.txt:",
        "  find_package(Atlas 0.27 REQUIRED COMPONENTS app)",
        "",
        "A consumer must use:",
        f"  - the same compiler and major version: {compiler}",
        f"  - the same triplet: {triplet}",
        "  - set(CMAKE_CXX_SCAN_FOR_MODULES OFF): Atlas is headers, and the scan needs a tool",
        "    the package cannot provide",
    ]
    if deployment:
        notes.append(f"  - a macOS deployment target of at least {deployment}")
    if triplet.endswith("-windows"):
        notes.append("  - a build type of the same kind (debug or not) as " + build_type)
        notes.append("  - and copy $<TARGET_RUNTIME_DLLS:target> beside each executable")
    notes.append("See docs/adr/0024-install-and-export.md, D9.")
    print("\n".join(notes), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
