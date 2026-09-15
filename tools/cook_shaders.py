#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile Atlas's shaders from HLSL to the formats each graphics backend accepts.

HLSL is the authoring language; see docs/adr/0006-shader-toolchain.md for why. The route is
HLSL to SPIR-V with glslang, then SPIR-V to Metal Shading Language with SPIRV-Cross. Both
tools are cross-platform and in the pinned dependency baseline, which the alternative
(Microsoft's DXC, and SDL_shadercross on top of it) is not: DXC has no macOS build at all,
so it cannot run on the machine this project is developed on.

The cooked outputs are committed. Building and running Atlas therefore needs no shader
toolchain; only changing a shader does. `--check` verifies the committed outputs match the
sources, which is what continuous integration runs.

Metal note: SPIRV-Cross renames the entry point to "main0". The manifest records the entry
point per format so the loader does not have to know that.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE_DIR = REPO_ROOT / "assets" / "source" / "shaders"
COOKED_DIR = REPO_ROOT / "assets" / "cooked" / "shaders"
MANIFEST = COOKED_DIR / "manifest.json"

# Bumped by hand when the cooking rules change in a way that invalidates existing outputs.
COOKER_VERSION = 1

STAGE_FROM_SUFFIX = {"vert": "vertex", "frag": "fragment"}


def find_tool(names: list[str]) -> str | None:
    for name in names:
        found = shutil.which(name)
        if found:
            return found
        # Homebrew installs these outside the default PATH on some setups.
        for prefix in ("/opt/homebrew/bin", "/usr/local/bin"):
            candidate = Path(prefix) / name
            if candidate.is_file():
                return str(candidate)
    return None


def tool_version(executable: str, args: list[str]) -> str:
    try:
        result = subprocess.run(
            [executable, *args], capture_output=True, text=True, timeout=30, check=False
        )
        return (result.stdout + result.stderr).strip().splitlines()[0]
    except (OSError, subprocess.SubprocessError, IndexError):
        return "unknown"


def source_hash(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()[:16]


def discover() -> list[tuple[Path, str, str]]:
    """Every shader source, as (path, name, stage)."""
    found = []
    for path in sorted(SOURCE_DIR.glob("*.hlsl")):
        parts = path.name.split(".")
        if len(parts) != 3 or parts[1] not in STAGE_FROM_SUFFIX:
            print(f"skipping {path.name}: expected <name>.<vert|frag>.hlsl", file=sys.stderr)
            continue
        found.append((path, parts[0], STAGE_FROM_SUFFIX[parts[1]]))
    return found


def cook_one(glslang: str, spirv_cross: str, path: Path, name: str, stage: str,
             out_dir: Path) -> dict:
    stage_flag = "vert" if stage == "vertex" else "frag"
    spv_path = out_dir / f"{name}.{stage_flag}.spv"
    msl_path = out_dir / f"{name}.{stage_flag}.msl"

    # HLSL to SPIR-V. -D selects HLSL input; the Vulkan target is what SDL's Vulkan backend
    # consumes, and it is also the input SPIRV-Cross expects.
    subprocess.run(
        [glslang, "-D", "-e", "main", "--target-env", "vulkan1.0", "-S", stage_flag,
         "-o", str(spv_path), str(path)],
        check=True, capture_output=True, text=True,
    )

    # SPIR-V to Metal Shading Language, shipped as source. SDL compiles it when the device
    # is created, which is what makes a Metal compiler unnecessary on this machine.
    subprocess.run(
        [spirv_cross, "--msl", "--msl-version", "20100", str(spv_path), "--output", str(msl_path)],
        check=True, capture_output=True, text=True,
    )

    return {
        "name": name,
        "stage": stage,
        "source": path.name,
        "source_hash": source_hash(path),
        "outputs": {
            "spirv": {"file": spv_path.name, "entry_point": "main"},
            # SPIRV-Cross always renames the entry point.
            "msl": {"file": msl_path.name, "entry_point": "main0"},
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="verify the committed outputs are current; change nothing")
    args = parser.parse_args()

    glslang = find_tool(["glslangValidator", "glslang"])
    spirv_cross = find_tool(["spirv-cross"])

    sources = discover()
    if not sources:
        print("no shader sources found")
        return 0

    if not glslang or not spirv_cross:
        missing = []
        if not glslang:
            missing.append("glslang")
        if not spirv_cross:
            missing.append("spirv-cross")
        message = (f"shader toolchain not found: {', '.join(missing)}. "
                   f"Install with: brew install {' '.join(missing)}")
        if args.check:
            # Not a failure. The committed outputs are what the build uses, and a machine
            # without the toolchain is a machine that cannot have changed them.
            print(f"skipping shader check: {message}")
            return 0
        print(message, file=sys.stderr)
        return 1

    versions = {
        "glslang": tool_version(glslang, ["--version"]),
        "spirv-cross": tool_version(spirv_cross, ["--version"]),
        "cooker": str(COOKER_VERSION),
    }

    out_dir = COOKED_DIR
    if args.check:
        out_dir = Path(REPO_ROOT / "build" / "shader-check")
        if out_dir.exists():
            shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    entries = []
    for path, name, stage in sources:
        try:
            entries.append(cook_one(glslang, spirv_cross, path, name, stage, out_dir))
        except subprocess.CalledProcessError as error:
            print(f"failed to cook {path.name}:", file=sys.stderr)
            print(error.stdout or "", file=sys.stderr)
            print(error.stderr or "", file=sys.stderr)
            return 1

    manifest = {"cooker_version": COOKER_VERSION, "tools": versions, "shaders": entries}

    if args.check:
        if not MANIFEST.is_file():
            print("no committed shader manifest; run tools/cook_shaders.py", file=sys.stderr)
            return 1
        committed = json.loads(MANIFEST.read_text(encoding="utf-8"))

        # Compare the sources and the outputs, not the tool version strings: a different
        # patch release of glslang produces identical output and should not fail a build.
        def comparable(data: dict) -> list:
            return sorted(
                (entry["name"], entry["stage"], entry["source_hash"]) for entry in data["shaders"]
            )

        if comparable(committed) != comparable(manifest):
            print("committed shaders are out of date; run tools/cook_shaders.py",
                  file=sys.stderr)
            return 1

        for entry in manifest["shaders"]:
            for output in entry["outputs"].values():
                fresh = out_dir / output["file"]
                stored = COOKED_DIR / output["file"]
                if not stored.is_file():
                    print(f"missing committed output {output['file']}", file=sys.stderr)
                    return 1
                if fresh.read_bytes() != stored.read_bytes():
                    print(f"{output['file']} differs from a fresh build; "
                          f"run tools/cook_shaders.py", file=sys.stderr)
                    return 1

        print(f"shaders are current: {len(entries)} compiled, outputs match")
        return 0

    MANIFEST.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"cooked {len(entries)} shaders into {COOKED_DIR.relative_to(REPO_ROOT)}")
    for entry in entries:
        print(f"  {entry['name']} ({entry['stage']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
