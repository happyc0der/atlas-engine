#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build mods written in C, and check that committed modules are current (ADR-0023, ADR-0024).

`tools/gen_mods.py` writes its demonstration modules byte by byte, so their check compares bytes
this repository decides. A mod written in C is compiled, and **its bytes are the compiler's**.
CLAUDE.md: "A `--check` that compares bytes must compare bytes only this repository decides." So
this check compares four things instead, and each says why:

1. **The inputs, always.** Every source and header a module is built from is hashed into the
   manifest, with the flags. A source edited without rebuilding fails on any machine, with or
   without a compiler, because those hashes are the project's own.
2. **The committed module against the manifest, always.** The manifest records the hash of the
   module it describes, so a module replaced by hand fails too.
3. **The bytes of a rebuild, only where the toolchain matches the manifest's exactly.** Anywhere
   else the rebuild still happens, so a source that no longer compiles fails, but the byte
   comparison is skipped and says so, naming both versions — the shader cooker's rule.
4. **Behaviour, wherever a toolchain and something to play the mod in both exist.** Given
   `--play-with BINARY`, the rebuilt module and the committed one each run in that binary as the
   list's `play` entry says, and what the list says to compare must come out the same. This is
   what verifies a module on a machine whose compiler cannot reproduce its bytes.

**Which mods, and where, is a list, not this script** (ADR-0024 D8). A project names its mods in a
JSON file, with paths relative to `--root`:

    {"format": "atlas-mod-list", "version": 1,
     "output_dir": "assets/mods", "manifest": "assets/mods/build_manifest.json",
     "mods": [{"output": "painter.wasm", "sources": [...], "inputs": [...],
               "include_dirs": [...], "initial_memory": 65536, "max_memory": 131072,
               "stack_size": 16384,
               "play": {"args": ["--headless", "--mod", "{module}", "--mods-dir", "{mods_dir}"],
                        "compare": ["final tick=\\d+ state hash=0x[0-9a-f]+"]}}]}

`sources` are compiled; `inputs` are headers, hashed but not compiled. **The guest's one header
from the engine is `atlas_mod.h`, and it is the only one a mod can reach**: the compiler is given
an include directory holding that header and nothing else, taken from this Atlas — beside this
script in the source tree, or in the install prefix when this script is the installed copy in
`share/atlas/tools`. The manifest records it as `atlas/script/atlas_mod.h`, so a project that moves
to an Atlas whose guest interface changed fails the check until it rebuilds, which is the point.

Usage:
    python3 tools/build_mods.py --mods assets/source/mods/mods.json            # build
    python3 tools/build_mods.py --mods assets/source/mods/mods.json --check    # verify
    python3 tools/build_mods.py --mods apps/chess/mods.json --check --play-with build/macos-debug/bin/atlas_chess

`--root` defaults to the current directory. The toolchain is clang and wasm-ld of LLVM major 23,
found as `$ATLAS_WASM_CLANG` and `$ATLAS_WASM_LD`, as `clang-23` and `wasm-ld-23` on the path
(Ubuntu's apt.llvm.org packages), or as Homebrew's `llvm@23` and `lld`.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent

#: The guest interface, under the name the manifest records it by.
GUEST_HEADER = "atlas/script/atlas_mod.h"

LLVM_MAJOR = 23

#: Pinned, never defaulted (ADR-0023 D2). `mvp` switches every optional feature off; the three
#: added back are ones the runtime is built with or accepts, and are what the first compiled mod
#: was proved to load with (M26). A compiler upgrade that changes its defaults cannot change these.
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



def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def find_guest_header(override: str | None) -> Path:
    """This Atlas's atlas_mod.h: in the source tree, or in the install this script came from."""
    candidates = ([Path(override) / GUEST_HEADER] if override else [
        SCRIPT_DIR.parent / "engine" / "script" / "include" / GUEST_HEADER,  # tools/ in the tree
        # .parent rather than .parents[2], which raises for a script at a shallow path such as
        # /src/tools — the Linux container's, where this was found — before either is looked at.
        SCRIPT_DIR.parent.parent.parent / "include" / GUEST_HEADER,  # <prefix>/share/atlas/tools
    ])
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise SystemExit("error: no atlas_mod.h found at " + " or ".join(map(str, candidates)))


def load_list(path: Path) -> dict:
    try:
        mod_list = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"error: cannot read the mod list {path}: {error}") from error
    if mod_list.get("format") != "atlas-mod-list" or mod_list.get("version") != 1:
        raise SystemExit(f"error: {path} is not an atlas-mod-list, version 1")
    for key in ("output_dir", "manifest", "mods"):
        if key not in mod_list:
            raise SystemExit(f"error: {path} has no '{key}'")
    for mod in mod_list["mods"]:
        for key in ("output", "sources", "inputs", "include_dirs", "initial_memory",
                    "max_memory", "stack_size"):
            if key not in mod:
                raise SystemExit(f"error: {path}: a mod has no '{key}'")
    return mod_list


class Project:
    """One mod list, resolved against its root and this Atlas's guest header."""

    def __init__(self, list_path: Path, root: Path, header: Path) -> None:
        self.list_path = list_path
        self.root = root
        self.header = header
        self.mod_list = load_list(list_path)
        self.output_dir = root / self.mod_list["output_dir"]
        self.manifest = root / self.mod_list["manifest"]
        self.mods = self.mod_list["mods"]

    def rebuild_hint(self) -> str:
        try:
            shown = self.list_path.relative_to(self.root)
        except ValueError:
            shown = self.list_path
        return f"run tools/build_mods.py --mods {shown}"

    def input_hashes(self, mod: dict) -> dict[str, str]:
        files = sorted(set(mod["sources"]) | set(mod["inputs"]))
        hashes = {name: sha256(self.root / name) for name in files}
        hashes[GUEST_HEADER] = sha256(self.header)
        return hashes

    def recipe(self, mod: dict) -> dict:
        """Everything about how a module is built that is the project's to decide."""
        return {
            "compile_flags": COMPILE_FLAGS,
            "link_flags": link_flags(mod),
            "include_dirs": mod["include_dirs"],
            "inputs": self.input_hashes(mod),
        }

    def build(self, mod: dict, tools: tuple[str, str], out: Path) -> None:
        clang, ld = tools
        with tempfile.TemporaryDirectory() as scratch:
            # The engine's include directory holds exactly one header for a guest.
            guest = Path(scratch) / "guest-include"
            (guest / GUEST_HEADER).parent.mkdir(parents=True)
            shutil.copyfile(self.header, guest / GUEST_HEADER)
            includes = [f"-I{guest}"] + [f"-I{self.root / d}" for d in mod["include_dirs"]]
            objects = []
            for source in mod["sources"]:
                obj = Path(scratch) / (Path(source).stem + ".o")
                subprocess.run([clang, *COMPILE_FLAGS, *includes, "-c", str(self.root / source),
                                "-o", str(obj)], check=True)
                objects.append(str(obj))
            subprocess.run([ld, *link_flags(mod), *objects, "-o", str(out)], check=True)

    def play(self, binary: str, mods_dir: Path, mod: dict) -> tuple[str, ...]:
        """Run `mod` as its list says; return what the list says to compare."""
        spec = mod["play"]
        args = [arg.format(module=mod["output"], mods_dir=mods_dir) for arg in spec["args"]]
        result = subprocess.run([binary, *args], capture_output=True, text=True, cwd=self.root,
                                timeout=600, check=False)
        output = result.stdout + result.stderr
        if result.returncode != 0:
            raise SystemExit(f"error: {mod['output']} did not run to the end in {binary}:\n"
                             f"{output}")
        found = []
        for pattern in spec["compare"]:
            found += [match.group(0) for match in re.finditer(pattern, output, re.MULTILINE)]
        if not found:
            raise SystemExit(f"error: running {mod['output']} printed nothing the list compares")
        return tuple(found)


def write_manifest(path: Path, manifest: dict) -> None:
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def command_build(project: Project) -> int:
    tools = toolchain()
    if tools is None:
        print(f"error: no LLVM {LLVM_MAJOR} clang and wasm-ld found; see this script's "
              "docstring", file=sys.stderr)
        return 1
    versions = {"clang": version_of(tools[0]), "wasm-ld": version_of(tools[1])}
    check_major(versions)
    manifest = {"format": "atlas-mod-build", "version": 2, "tools": versions, "mods": {}}
    project.output_dir.mkdir(parents=True, exist_ok=True)
    for mod in project.mods:
        out = project.output_dir / mod["output"]
        project.build(mod, tools, out)
        manifest["mods"][mod["output"]] = {**project.recipe(mod), "module_sha256": sha256(out),
                                           "module_bytes": out.stat().st_size}
        print(f"built {mod['output']}: {out.stat().st_size} bytes")
    write_manifest(project.manifest, manifest)
    return 0


def command_check(project: Project, play_with: str | None) -> int:
    if not project.manifest.exists():
        print(f"error: {project.manifest} is missing; {project.rebuild_hint()}", file=sys.stderr)
        return 1
    manifest = json.loads(project.manifest.read_text(encoding="utf-8"))
    if manifest.get("format") != "atlas-mod-build" or manifest.get("version") != 2:
        print(f"error: {project.manifest} is not an atlas-mod-build manifest, version 2; "
              f"{project.rebuild_hint()}", file=sys.stderr)
        return 1
    failed = False

    # 1 and 2: what the project decides, checked on every machine.
    for mod in project.mods:
        name = mod["output"]
        recorded = manifest["mods"].get(name)
        if recorded is None:
            print(f"error: {name} is not in the manifest; {project.rebuild_hint()}",
                  file=sys.stderr)
            failed = True
            continue
        current = project.recipe(mod)
        for key in ("compile_flags", "link_flags", "include_dirs", "inputs"):
            if recorded.get(key) != current[key]:
                print(f"error: {name} is stale: its {key.replace('_', ' ')} changed since it was "
                      f"built; {project.rebuild_hint()}", file=sys.stderr)
                failed = True
        committed = project.output_dir / name
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
        for mod in project.mods:
            rebuilt = Path(scratch) / mod["output"]
            project.build(mod, tools, rebuilt)
            if same_toolchain and sha256(rebuilt) != manifest["mods"][mod["output"]]["module_sha256"]:
                print(f"error: {mod['output']} rebuilt with the manifest's own toolchain differs "
                      f"from the committed module; {project.rebuild_hint()}", file=sys.stderr)
                failed = True
            # 4: behaviour, where there is something to play it in.
            if play_with is None or failed:
                continue
            if "play" not in mod:
                print(f"note: {mod['output']} has no play entry; not comparing behaviour",
                      file=sys.stderr)
                continue
            committed_run = project.play(play_with, project.output_dir, mod)
            rebuilt_run = project.play(play_with, Path(scratch), mod)
            if committed_run != rebuilt_run:
                print(f"error: {mod['output']} rebuilt here behaves differently from the "
                      "committed module:\n  committed: " + " | ".join(committed_run) +
                      "\n  rebuilt:   " + " | ".join(rebuilt_run), file=sys.stderr)
                failed = True
            else:
                print(f"{mod['output']}: the rebuilt module behaves as the committed one "
                      f"({committed_run[-1]})")
    if failed:
        return 1
    count = len(project.mods)
    if same_toolchain:
        print(f"mods are current: {count} module(s) rebuilt byte for byte")
    else:
        print(f"mods are current: {count} module(s) rebuilt, bytes not compared")
        print("note: this toolchain differs from the one that built the committed modules, so "
              "their bytes are the compiler's and are not compared here.", file=sys.stderr)
        print(f"  committed: {manifest['tools']}", file=sys.stderr)
        print(f"  here:      {versions}", file=sys.stderr)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--mods", required=True, help="the project's mod list, a JSON file")
    parser.add_argument("--root", help="what the list's paths are relative to; default: here")
    parser.add_argument("--atlas-include",
                        help="an Atlas include directory to take atlas_mod.h from, instead of "
                             "this script's own Atlas")
    parser.add_argument("--check", action="store_true",
                        help="verify the committed modules instead of rebuilding them")
    parser.add_argument("--play-with", metavar="BINARY",
                        help="with --check: also compare how the rebuilt and committed modules "
                             "behave, in this binary")
    # An empty argument is what CTest passes where the binary is not built; see the tests'
    # registration.
    args = parser.parse_args([arg for arg in sys.argv[1:] if arg])
    if args.play_with and not Path(args.play_with).exists():
        # Named but not built yet — a configure that has not been followed by a build. Said, and
        # the behavioural comparison skipped, rather than failed for a reason about the build.
        print(f"note: {args.play_with} does not exist; not comparing behaviour", file=sys.stderr)
        args.play_with = None
    root = Path(args.root).resolve() if args.root else Path.cwd()
    list_path = Path(args.mods)
    if not list_path.is_absolute():
        list_path = root / list_path
    project = Project(list_path, root, find_guest_header(args.atlas_include))
    return command_check(project, args.play_with) if args.check else command_build(project)


if __name__ == "__main__":
    sys.exit(main())
