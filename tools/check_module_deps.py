#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Enforce Atlas module boundaries.

Three checks, all reading the same allow-list from cmake/ModuleGraph.cmake so that the
table stays the single source of truth:

1. Include edges. Every `#include <atlas/X/...>` inside module Y must be permitted by the
   allow-list. This catches a dependency added without going through atlas_add_module.
2. Public header purity. A public header must not include a third-party library, except
   where the table records an ADR-backed exception.
3. Exceptions. `throw` and `catch` must not appear in engine code outside an explicit
   allow-list of boundary wrappers, per ADR-0005.

CMake tolerates static-library cycles at link time, and the allow-list is validated for
cycles at configure time by ModuleGraph.cmake itself, so cycles are covered there.

Exit code 0 when clean, 1 when a violation is found.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ENGINE_DIR = REPO_ROOT / "engine"
MODULE_GRAPH = REPO_ROOT / "cmake" / "ModuleGraph.cmake"

SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".cc", ".cxx", ".inl"}

# Files permitted to contain `throw` or `catch`, with the reason. ADR-0005 requires that
# every such site be a deliberate third-party or standard-library boundary.
EXCEPTION_ALLOWLIST: dict[str, str] = {
    "engine/tasks/src/worker_pool.cpp": (
        "A thread boundary. An exception escaping a parallel_for body has nowhere to go: the "
        "caller is blocked in another frame, and letting it unwind out of a worker would call "
        "std::terminate with no diagnostic. The wrapper reports what was thrown and ends the "
        "process, which is the treatment ADR-0005 gives allocation failure."
    ),
}

# Third-party include prefixes that indicate a dependency leaking into a public header.
THIRD_PARTY_PREFIXES = (
    "tracy/",
    "SDL3/",
    "SDL3_",
    "entt/",
    "imgui",
    "catch2/",
    "stb_",
)

ATLAS_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]atlas/([A-Za-z0-9_]+)/')
ANY_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
THROW_CATCH_RE = re.compile(r"\b(throw|catch)\b")


def parse_module_graph() -> tuple[dict[str, set[str]], dict[str, set[str]]]:
    """Read the dependency allow-list and the public third-party exceptions."""
    if not MODULE_GRAPH.is_file():
        sys.exit(f"error: cannot find {MODULE_GRAPH}")

    text = MODULE_GRAPH.read_text(encoding="utf-8")

    deps: dict[str, set[str]] = {}
    for match in re.finditer(
        r'set\(ATLAS_MODULE_DEPS_([A-Za-z0-9_]+)\s+"([^"]*)"', text
    ):
        module, value = match.group(1), match.group(2)
        deps[module] = {d for d in value.split(";") if d}

    public: dict[str, set[str]] = {}
    for match in re.finditer(
        r'set\(ATLAS_PUBLIC_THIRDPARTY_([A-Za-z0-9_]+)\s+"([^"]*)"', text
    ):
        module, value = match.group(1), match.group(2)
        public[module] = {d for d in value.split(";") if d}

    if not deps:
        sys.exit(f"error: no module dependencies parsed from {MODULE_GRAPH}")

    return deps, public


def module_of(path: Path) -> str | None:
    """The module a file belongs to, from its path under engine/."""
    try:
        relative = path.relative_to(ENGINE_DIR)
    except ValueError:
        return None
    return relative.parts[0] if relative.parts else None


def is_public_header(path: Path) -> bool:
    return "include" in path.parts and path.suffix in {".hpp", ".h", ".inl"}


def is_test_file(path: Path) -> bool:
    return "tests" in path.parts


def source_files() -> list[Path]:
    if not ENGINE_DIR.is_dir():
        return []
    return sorted(
        p
        for p in ENGINE_DIR.rglob("*")
        if p.is_file() and p.suffix in SOURCE_SUFFIXES
    )


def check_include_edges(
    path: Path, module: str, deps: dict[str, set[str]], lines: list[str]
) -> list[str]:
    """Every atlas/<other>/ include must be an allowed edge."""
    problems: list[str] = []
    permitted = deps.get(module, set())

    for number, line in enumerate(lines, start=1):
        match = ATLAS_INCLUDE_RE.match(line)
        if match is None:
            continue
        target = match.group(1)
        if target == module or target in permitted:
            continue
        # Tests may use anything their own module may use, plus their own module.
        problems.append(
            f"{path.relative_to(REPO_ROOT).as_posix()}:{number}: "
            f"module '{module}' includes <atlas/{target}/...>, which is not a permitted "
            f"dependency. Permitted: {sorted(permitted) or 'none'}. "
            f"Moving this boundary means editing cmake/ModuleGraph.cmake."
        )
    return problems


def check_public_header_purity(
    path: Path, module: str, public: dict[str, set[str]], lines: list[str]
) -> list[str]:
    """A public header must not leak a third-party dependency."""
    problems: list[str] = []
    allowed = public.get(module, set())

    for number, line in enumerate(lines, start=1):
        match = ANY_INCLUDE_RE.match(line)
        if match is None:
            continue
        included = match.group(1)
        if not included.startswith(THIRD_PARTY_PREFIXES):
            continue
        library = included.split("/", maxsplit=1)[0].rstrip("_").lower()
        if library in allowed:
            continue
        problems.append(
            f"{path.relative_to(REPO_ROOT).as_posix()}:{number}: "
            f"public header includes third-party '{included}'. Third-party libraries are "
            f"private to a module's sources unless an ADR records an exception in "
            f"cmake/ModuleGraph.cmake."
        )
    return problems


def check_exceptions(path: Path, lines: list[str]) -> list[str]:
    """throw and catch belong only in documented boundary wrappers (ADR-0005)."""
    # as_posix, not str: on Windows str gives backslashes and no allowlist key ever matches,
    # so a documented boundary wrapper is reported as a violation there and nowhere else. Found
    # by continuous integration on the one platform this cannot be tested on locally.
    relative = path.relative_to(REPO_ROOT).as_posix()
    if relative in EXCEPTION_ALLOWLIST or is_test_file(path):
        return []

    problems: list[str] = []
    for number, line in enumerate(lines, start=1):
        stripped = line.strip()
        # Skip comments: prose about exceptions is not an exception.
        if stripped.startswith(("//", "*", "/*", "///")):
            continue
        if THROW_CATCH_RE.search(line):
            problems.append(
                f"{relative}:{number}: "
                f"'throw' or 'catch' in engine code. ADR-0005 permits these only in "
                f"documented boundary wrappers; add this file to EXCEPTION_ALLOWLIST in "
                f"tools/check_module_deps.py with a reason if it is one."
            )
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--quiet", action="store_true", help="print nothing when the tree is clean"
    )
    args = parser.parse_args()

    deps, public = parse_module_graph()
    problems: list[str] = []
    checked = 0

    for path in source_files():
        module = module_of(path)
        if module is None:
            continue

        if module not in deps:
            problems.append(
                f"{path.relative_to(REPO_ROOT).as_posix()}: file lives under engine/{module}/, which "
                f"is not a module in cmake/ModuleGraph.cmake."
            )
            continue

        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        checked += 1

        if not is_test_file(path):
            problems.extend(check_include_edges(path, module, deps, lines))
        if is_public_header(path):
            problems.extend(check_public_header_purity(path, module, public, lines))
        problems.extend(check_exceptions(path, lines))

    if problems:
        print("Module boundary violations:\n", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}\n", file=sys.stderr)
        print(f"{len(problems)} violation(s) in {checked} file(s).", file=sys.stderr)
        return 1

    if not args.quiet:
        print(f"Module boundaries clean: {checked} file(s), {len(deps)} module(s) declared.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
