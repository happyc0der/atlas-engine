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

Binding conventions matter from the moment a shader reads anything. SDL_GPU requires
resources in a particular order per stage, and the HLSL registers here are authored to
produce it: vertex uniform buffers at `b[n] space1`, fragment textures and samplers at
`t[n] s[n] space2`, fragment uniform buffers at `b[n] space3`. glslang maps an HLSL register
space onto a SPIR-V descriptor set, and `--msl-decoration-binding` makes SPIRV-Cross use
those same numbers for Metal's `[[buffer]]`, `[[texture]]` and `[[sampler]]` indices rather
than allocating its own. Verified against SDL's documented order, not assumed.

SDL also wants the resource counts declared when a shader is created, and a count that
disagrees with the shader produces a driver-level failure with no useful message. They are
therefore read out of the compiled shader by reflection and written into a generated header,
so that they cannot drift from the code they describe.

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
COOKER_VERSION = 2

GENERATED_HEADER = COOKED_DIR / "shader_manifest.hpp"

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


def reflect(spirv_cross: str, spv_path: Path) -> dict:
    """Resource counts and vertex inputs, read out of the compiled shader."""
    result = subprocess.run(
        [spirv_cross, str(spv_path), "--reflect"],
        check=True, capture_output=True, text=True,
    )
    data = json.loads(result.stdout)

    # SDL counts a sampled texture and its sampler as one "sampler" slot, bound together by
    # SDL_BindGPU*Samplers. Separate images and separate samplers therefore have to agree.
    images = len(data.get("separate_images", []))
    samplers = len(data.get("separate_samplers", []))
    if images != samplers:
        raise ValueError(
            f"{spv_path.name}: {images} textures but {samplers} samplers. SDL binds them in "
            f"pairs, so each sampled texture needs exactly one sampler."
        )

    return {
        "samplers": images,
        "storage_textures": len(data.get("images", [])),
        "storage_buffers": len(data.get("ssbos", [])),
        "uniform_buffers": len(data.get("ubos", [])),
        "inputs": [
            {"location": entry["location"], "type": entry["type"]}
            for entry in sorted(data.get("inputs", []), key=lambda e: e["location"])
        ],
    }


def cook_one(glslang: str, spirv_cross: str, path: Path, name: str, stage: str,
             out_dir: Path) -> dict:
    stage_flag = "vert" if stage == "vertex" else "frag"
    spv_path = out_dir / f"{name}.{stage_flag}.spv"
    msl_path = out_dir / f"{name}.{stage_flag}.msl"

    # HLSL to SPIR-V. -D selects HLSL input; the Vulkan target is what SDL's Vulkan backend
    # consumes, and it is also the input SPIRV-Cross expects. An HLSL register space becomes
    # a SPIR-V descriptor set, which is how the authored registers reach SDL's convention.
    subprocess.run(
        [glslang, "-D", "-e", "main", "--target-env", "vulkan1.0", "-S", stage_flag,
         "-o", str(spv_path), str(path)],
        check=True, capture_output=True, text=True,
    )

    # SPIR-V to Metal Shading Language, shipped as source. SDL compiles it when the device
    # is created, which is what makes a Metal compiler unnecessary on this machine.
    #
    # --msl-decoration-binding is not optional: without it SPIRV-Cross allocates its own
    # Metal indices, and they will not be the ones SDL binds against.
    subprocess.run(
        [spirv_cross, "--msl", "--msl-version", "20100", "--msl-decoration-binding",
         str(spv_path), "--output", str(msl_path)],
        check=True, capture_output=True, text=True,
    )

    resources = reflect(spirv_cross, spv_path)

    return {
        "name": name,
        "stage": stage,
        "source": path.name,
        "source_hash": source_hash(path),
        "resources": resources,
        "outputs": {
            "spirv": {"file": spv_path.name, "entry_point": "main"},
            # SPIRV-Cross always renames the entry point.
            "msl": {"file": msl_path.name, "entry_point": "main0"},
        },
    }


def write_header(manifest: dict, path: Path) -> None:
    """Emit the manifest as a header of compile-time constants.

    Generated rather than parsed at runtime: Atlas has no JSON reader, adding one to load
    four integers would be a dependency in search of a problem, and constants mean a shader
    that gains a resource breaks the build rather than the frame. M4 considered routing
    shaders through the asset registry and decided against it; see docs/DEFERRED.md.
    """
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "//",
        "// Generated by tools/cook_shaders.py. Do not edit.",
        "//",
        "// Resource counts are read out of the compiled shaders by reflection, so they",
        "// cannot disagree with the shaders they describe.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace atlas::shaders {",
        "",
        "struct ShaderInfo {",
        "    std::string_view name;",
        "    std::string_view stage;            ///< \"vertex\" or \"fragment\"",
        "    std::string_view spirv_file;",
        "    std::string_view spirv_entry_point;",
        "    std::string_view msl_file;",
        "    std::string_view msl_entry_point;",
        "    std::uint32_t samplers = 0;        ///< Sampled textures, each with its sampler",
        "    std::uint32_t storage_textures = 0;",
        "    std::uint32_t storage_buffers = 0;",
        "    std::uint32_t uniform_buffers = 0;",
        "};",
        "",
    ]

    for entry in manifest["shaders"]:
        resources = entry["resources"]
        identifier = f"k{entry['name'].title().replace('_', '')}{entry['stage'].title()}"
        lines += [
            f"inline constexpr ShaderInfo {identifier}{{",
            f'    .name = "{entry["name"]}",',
            f'    .stage = "{entry["stage"]}",',
            f'    .spirv_file = "{entry["outputs"]["spirv"]["file"]}",',
            f'    .spirv_entry_point = "{entry["outputs"]["spirv"]["entry_point"]}",',
            f'    .msl_file = "{entry["outputs"]["msl"]["file"]}",',
            f'    .msl_entry_point = "{entry["outputs"]["msl"]["entry_point"]}",',
            f'    .samplers = {resources["samplers"]},',
            f'    .storage_textures = {resources["storage_textures"]},',
            f'    .storage_buffers = {resources["storage_buffers"]},',
            f'    .uniform_buffers = {resources["uniform_buffers"]},',
            "};",
            "",
        ]

    identifiers = [
        f"k{entry['name'].title().replace('_', '')}{entry['stage'].title()}"
        for entry in manifest["shaders"]
    ]
    lines += [
        "/// Every cooked shader, so that one can be found by name and stage.",
        f"inline constexpr const ShaderInfo* kAll[] = {{",
    ]
    lines += [f"    &{identifier}," for identifier in identifiers]
    lines += [
        "};",
        "",
        "}  // namespace atlas::shaders",
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


# Vertex layouts the engine declares in C++, as {shader name: {location: type}}.
#
# This table is the other half of a contract that had no check at all. The renderer declares
# vertex attributes by location and format; the shader declares inputs by semantic, which
# glslang turns into the same locations. Nothing compared the two, so a field added on one side
# and forgotten on the other compiled cleanly and failed at the driver, which reports it — in
# the words of the RHI header — as "a driver-level failure with no useful message".
#
# The reflection needed to check it was already being collected and written into the manifest,
# and read by nothing. M13 grew the quad instance from 48 bytes to 64 and this is what makes
# that kind of change fail loudly next time.
#
# A shader absent from here is not checked, which is deliberate: the lab's cell shaders derive
# their geometry from vertex and instance indices and declare no vertex inputs at all.
DECLARED_VERTEX_INPUTS = {
    "sprite": {
        0: "vec2",  # corner position, per vertex
        1: "vec2",  # corner texture coordinate, per vertex
        2: "vec4",  # instance position and size
        3: "vec4",  # instance texture rectangle
        4: "vec4",  # instance colour
        5: "vec4",  # instance rotation and pivot
    },
}


def check_vertex_inputs(manifest: dict) -> list[str]:
    """Compare each vertex shader's declared inputs against the layout the engine declares."""
    problems = []
    for entry in manifest["shaders"]:
        if entry["stage"] != "vertex":
            continue
        expected = DECLARED_VERTEX_INPUTS.get(entry["name"])
        if expected is None:
            continue
        found = {item["location"]: item["type"]
                 for item in entry["resources"].get("inputs", [])}
        if found != expected:
            problems.append(
                f"{entry['name']}.vert declares inputs {sorted(found.items())}, and the engine "
                f"declares {sorted(expected.items())}. One side was changed without the other; "
                f"see DECLARED_VERTEX_INPUTS in tools/cook_shaders.py and the vertex layout in "
                f"the module that draws with this shader."
            )
    return problems


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

        # Byte-comparing the outputs is only meaningful when the same compiler produced both.
        #
        # It is not, here, and never was: this machine cooks with Homebrew's glslang and
        # continuous integration with the distribution's, which are different builds of
        # different versions. That went unnoticed until M13, because every shader until then
        # was simple enough that both produced identical SPIR-V — so the check was passing by
        # coincidence rather than by construction, and the first shader with real arithmetic
        # in it turned that coincidence into a failure that said the committed output was
        # stale when it was not.
        #
        # So the comparison is made where it means something and skipped, loudly, where it
        # does not. This is the same reasoning that already makes the whole check skip on a
        # machine with no toolchain: one that cannot reproduce the bytes cannot judge them.
        # The source hashes and the generated header are compared either way, because neither
        # depends on which compiler ran.
        same_toolchain = committed.get("tools") == manifest["tools"]
        if same_toolchain:
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
        else:
            for entry in manifest["shaders"]:
                for output in entry["outputs"].values():
                    if not (COOKED_DIR / output["file"]).is_file():
                        print(f"missing committed output {output['file']}", file=sys.stderr)
                        return 1
            print("note: this toolchain differs from the one that cooked the committed "
                  "outputs, so they are checked for presence and currency but not compared "
                  "byte for byte.", file=sys.stderr)
            print(f"  committed with: {committed.get('tools')}", file=sys.stderr)
            print(f"  present here:   {manifest['tools']}", file=sys.stderr)

        # Version-independent, so this runs whichever compiler is present: it is derived from
        # the reflection rather than from the compiled bytes.
        fresh_header = out_dir / "shader_manifest.hpp"
        write_header(manifest, fresh_header)
        if not GENERATED_HEADER.is_file():
            print("no committed shader header; run tools/cook_shaders.py", file=sys.stderr)
            return 1
        if fresh_header.read_text(encoding="utf-8") != GENERATED_HEADER.read_text(
                encoding="utf-8"):
            print("the generated shader header is out of date; run tools/cook_shaders.py",
                  file=sys.stderr)
            return 1

        input_problems = check_vertex_inputs(manifest)
        if input_problems:
            for problem in input_problems:
                print(problem, file=sys.stderr)
            return 1

        compared = "outputs, header and vertex layouts match" if same_toolchain else (
            "header and vertex layouts match; outputs present but not byte-compared")
        print(f"shaders are current: {len(entries)} compiled, {compared}")
        return 0

    # Checked when cooking as well as when verifying: whoever changes a shader is the person
    # best placed to fix the mismatch, and they find out now rather than in CI.
    input_problems = check_vertex_inputs(manifest)
    if input_problems:
        for problem in input_problems:
            print(problem, file=sys.stderr)
        return 1

    MANIFEST.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    write_header(manifest, GENERATED_HEADER)

    print(f"cooked {len(entries)} shaders into {COOKED_DIR.relative_to(REPO_ROOT)}")
    for entry in entries:
        resources = entry["resources"]
        used = ", ".join(f"{key}={value}" for key, value in resources.items()
                         if key != "inputs" and value)
        print(f"  {entry['name']} ({entry['stage']}){': ' + used if used else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
