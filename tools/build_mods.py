#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the mods written in C, and check that the committed modules are current (ADR-0023).

`tools/gen_mods.py` writes its three demonstration modules byte by byte, so their check compares
bytes this repository decides. A mod written in C is compiled, and **its bytes are the
compiler's**. CLAUDE.md: "A `--check` that compares bytes must compare bytes only this repository
decides." So this check compares three things instead, and each says why:

1. **The inputs, always.** Every source and header a module is built from is hashed into the
   manifest, with the flags. A source edited without rebuilding fails on any machine, with or
   without a compiler, because those hashes are this repository's.
2. **The committed module against the manifest, always.** The manifest records the hash of the
   module it describes, so a module replaced by hand fails too.
3. **The bytes of a rebuild, only where the toolchain matches the manifest's exactly.** Anywhere
   else the rebuild still happens, so a source that no longer compiles fails, but the byte
   comparison is skipped and says so, naming both versions. That is the shader cooker's rule
   (`tools/cook_shaders.py`), which ADR-0015 prescribed for a compiled mod before one existed.

A fourth comparison, of behaviour — the rebuilt module and the committed one playing the same
game — needs something that can run a mod, and arrives with the chess application in M26's
fourth slice.

Usage:
    python3 tools/build_mods.py            # build and write the committed module and manifest
    python3 tools/build_mods.py --check    # verify, rebuilding where a toolchain exists

The toolchain is clang and wasm-ld of LLVM major 23, found as `$ATLAS_WASM_CLANG` and
`$ATLAS_WASM_LD`, as `clang-23` and `wasm-ld-23` on the path (Ubuntu's apt.llvm.org packages),
or as Homebrew's `llvm@23` and `lld`.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUTPUT_DIR = ROOT / "assets" / "mods"
MANIFEST = OUTPUT_DIR / "build_manifest.json"

LLVM_MAJOR = 23

#: Pinned, never defaulted (ADR-0023 D2). `mvp` switches every optional feature off; the three
#: added back are ones the runtime is built with or accepts, and are what the proof in
#: `apps/chess/mod/tests` loads. A compiler upgrade that changes its defaults cannot change
#: these.
COMPILE_FLAGS = [
    "--target=wasm32",
    "-mcpu=mvp",
    "-mbulk-memory",
    "-msign-ext",
    "-mmutable-globals",
    "-O2",
    "-std=c17",
    "-ffreestanding",
    "-fno-builtin",
    "-nostdlib",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Wshadow",
    "-Wconversion",
    "-Wsign-conversion",
    "-Werror",
]

#: What a module is, and what it is built from. `inputs` are hashed but not compiled: headers.
MODS = [
    {
        "output": "chess_opponent.wasm",
        "sources": [
            "apps/chess/mod/freestanding.c",
            "apps/chess/mod/rules.c",
            "apps/chess/mod/opponent.c",
        ],
        "inputs": [
            "engine/script/include/atlas/script/atlas_mod.h",
            "apps/chess/mod/rules.h",
            "apps/chess/sim/include/atlas/chess/mod_view.h",
        ],
        "include_dirs": ["engine/script/include", "apps/chess/sim/include", "apps/chess/mod"],
        # Pages of 64 KiB. The maximum is declared because the loader refuses a module without
        # one, and is far below the runtime's sixteen-mebibyte cap.
        "initial_memory": 2 * 65536,
        "max_memory": 16 * 65536,
        # Within the runtime's 64 KiB stack limit, with room for its own frames.
        "stack_size": 32768,
    },
]


def link_flags(mod: dict) -> list[str]:
    return [
        "--no-entry",
        "--export=mod_init",
        "--export=mod_tick",
        "--export=mod_shutdown",
        f"--initial-memory={mod['initial_memory']}",
        f"--max-memory={mod['max_memory']}",
        "-z",
        f"stack-size={mod['stack_size']}",
        # No names, no debug information, no producers section: nothing in the module that
        # records where or with what it was built, beyond what its code already is.
        "--strip-all",
    ]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def find_tool(env: str, names: list[str]) -> str | None:
    override = os.environ.get(env)
    if override:
        # Named explicitly, so a wrong path is an answer rather than a reason to look elsewhere:
        # it means "no toolchain here", and the check says so instead of crashing on it.
        return override if Path(override).exists() else None
    for name in names:
        found = shutil.which(name) if "/" not in name else (name if Path(name).exists() else None)
        if found:
            return found
    return None


def toolchain() -> tuple[str, str] | None:
    clang = find_tool("ATLAS_WASM_CLANG",
                      [f"clang-{LLVM_MAJOR}", f"/opt/homebrew/opt/llvm@{LLVM_MAJOR}/bin/clang"])
    ld = find_tool("ATLAS_WASM_LD", [f"wasm-ld-{LLVM_MAJOR}", "/opt/homebrew/opt/lld/bin/wasm-ld"])
    if clang is None or ld is None:
        return None
    return clang, ld


def version_of(tool: str) -> str:
    result = subprocess.run([tool, "--version"], capture_output=True, text=True, check=True)
    return result.stdout.splitlines()[0].strip()


def check_major(versions: dict[str, str]) -> None:
    for name, text in versions.items():
        if f" {LLVM_MAJOR}." not in text:
            raise SystemExit(f"error: {name} is '{text}', and mods are built with LLVM "
                             f"{LLVM_MAJOR} (ADR-0023)")


def input_hashes(mod: dict) -> dict[str, str]:
    files = sorted(set(mod["sources"]) | set(mod["inputs"]))
    return {name: sha256(ROOT / name) for name in files}


def recipe(mod: dict) -> dict:
    """Everything about how a module is built that is this repository's to decide."""
    return {
        "compile_flags": COMPILE_FLAGS,
        "link_flags": link_flags(mod),
        "include_dirs": mod["include_dirs"],
        "inputs": input_hashes(mod),
    }


def build(mod: dict, tools: tuple[str, str], out: Path) -> None:
    clang, ld = tools
    with tempfile.TemporaryDirectory() as scratch:
        objects = []
        for source in mod["sources"]:
            obj = Path(scratch) / (Path(source).stem + ".o")
            includes = [f"-I{ROOT / d}" for d in mod["include_dirs"]]
            subprocess.run([clang, *COMPILE_FLAGS, *includes, "-c", str(ROOT / source), "-o",
                            str(obj)], check=True)
            objects.append(str(obj))
        subprocess.run([ld, *link_flags(mod), *objects, "-o", str(out)], check=True)


def write_manifest(manifest: dict) -> None:
    MANIFEST.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def command_build() -> int:
    tools = toolchain()
    if tools is None:
        print(f"error: no LLVM {LLVM_MAJOR} clang and wasm-ld found; see this script's "
              "docstring", file=sys.stderr)
        return 1
    versions = {"clang": version_of(tools[0]), "wasm-ld": version_of(tools[1])}
    check_major(versions)
    manifest = {"format": "atlas-mod-build", "version": 1, "tools": versions, "mods": {}}
    for mod in MODS:
        out = OUTPUT_DIR / mod["output"]
        build(mod, tools, out)
        manifest["mods"][mod["output"]] = {**recipe(mod), "module_sha256": sha256(out),
                                           "module_bytes": out.stat().st_size}
        print(f"built {mod['output']}: {out.stat().st_size} bytes")
    write_manifest(manifest)
    return 0


def command_check() -> int:
    if not MANIFEST.exists():
        print(f"error: {MANIFEST.relative_to(ROOT)} is missing; run tools/build_mods.py",
              file=sys.stderr)
        return 1
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    failed = False

    # 1 and 2: what this repository decides, checked on every machine.
    for mod in MODS:
        name = mod["output"]
        recorded = manifest["mods"].get(name)
        if recorded is None:
            print(f"error: {name} is not in the manifest; run tools/build_mods.py",
                  file=sys.stderr)
            failed = True
            continue
        current = recipe(mod)
        for key in ("compile_flags", "link_flags", "include_dirs", "inputs"):
            if recorded.get(key) != current[key]:
                print(f"error: {name} is stale: its {key.replace('_', ' ')} changed since it was "
                      f"built; run tools/build_mods.py", file=sys.stderr)
                failed = True
        committed = OUTPUT_DIR / name
        if not committed.exists() or sha256(committed) != recorded.get("module_sha256"):
            print(f"error: {name} is not the module the manifest describes", file=sys.stderr)
            failed = True
    if failed:
        return 1

    # 3: a rebuild, where there is a toolchain; bytes compared only on the same one.
    tools = toolchain()
    if tools is None:
        print(f"skipping the rebuild: no LLVM {LLVM_MAJOR} clang and wasm-ld here. The inputs "
              "and the committed modules match the manifest.")
        return 0
    versions = {"clang": version_of(tools[0]), "wasm-ld": version_of(tools[1])}
    check_major(versions)
    same_toolchain = versions == manifest["tools"]
    with tempfile.TemporaryDirectory() as scratch:
        for mod in MODS:
            rebuilt = Path(scratch) / mod["output"]
            build(mod, tools, rebuilt)
            if same_toolchain and sha256(rebuilt) != manifest["mods"][mod["output"]]["module_sha256"]:
                print(f"error: {mod['output']} rebuilt with the manifest's own toolchain differs "
                      "from the committed module; run tools/build_mods.py", file=sys.stderr)
                failed = True
    if failed:
        return 1
    if same_toolchain:
        print(f"mods are current: {len(MODS)} module(s) rebuilt byte for byte")
    else:
        print(f"mods are current: {len(MODS)} module(s) rebuilt, bytes not compared")
        print("note: this toolchain differs from the one that built the committed modules, so "
              "their bytes are the compiler's and are not compared here.", file=sys.stderr)
        print(f"  committed: {manifest['tools']}", file=sys.stderr)
        print(f"  here:      {versions}", file=sys.stderr)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="verify the committed modules instead of rebuilding them")
    args = parser.parse_args()
    return command_check() if args.check else command_build()


if __name__ == "__main__":
    sys.exit(main())
