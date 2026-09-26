#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that every first-party file carries an SPDX licence identifier.

Atlas is GPL-3.0-or-later. A file without the identifier is ambiguous the moment it is
copied out of the repository, which is exactly when the licence matters.

Exit code 0 when clean, 1 when a file is missing the header. --fix inserts it.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
IDENTIFIER = "SPDX-License-Identifier: GPL-3.0-or-later"

# Directories that are checked. Anything outside these is third-party or generated.
SEARCH_DIRS = ("engine", "apps", "tests", "benchmarks", "tools", "cmake")

# Suffix to comment prefix.
COMMENT_PREFIX = {
    ".cpp": "//",
    ".hpp": "//",
    ".h": "//",
    ".cc": "//",
    ".cxx": "//",
    ".inl": "//",
    ".in": "//",
    ".cmake": "#",
    ".py": "#",
    ".sh": "#",
}

EXCLUDE_PARTS = {"build", "external", ".cache", "vcpkg_installed", "generated"}

# How many lines from the top to search. The identifier belongs at the top; allowing a
# shebang, an encoding line, and a blank line is enough slack.
HEADER_LINES = 5


def candidate_files() -> list[Path]:
    files: list[Path] = []
    for directory in SEARCH_DIRS:
        root = REPO_ROOT / directory
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            if EXCLUDE_PARTS & set(path.parts):
                continue
            suffix = path.suffix
            # CMakeLists.txt has no informative suffix.
            if path.name == "CMakeLists.txt":
                files.append(path)
            elif suffix in COMMENT_PREFIX:
                files.append(path)
    return sorted(files)


def comment_prefix_for(path: Path) -> str:
    if path.name == "CMakeLists.txt":
        return "#"
    if path.suffix == ".in":
        # A template comments in the language it generates: build_info.cpp.in is C++,
        # AtlasConfig.cmake.in is CMake. The suffix beneath ".in" says which.
        return COMMENT_PREFIX.get(Path(path.stem).suffix, "//")
    return COMMENT_PREFIX.get(path.suffix, "#")


def has_identifier(path: Path) -> bool:
    try:
        with path.open(encoding="utf-8", errors="replace") as handle:
            for index, line in enumerate(handle):
                if index >= HEADER_LINES:
                    return False
                if IDENTIFIER in line:
                    return True
    except OSError as error:
        print(f"warning: cannot read {path}: {error}", file=sys.stderr)
    return False


def insert_identifier(path: Path) -> None:
    prefix = comment_prefix_for(path)
    header = f"{prefix} {IDENTIFIER}\n"
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)

    # Keep a shebang first: it must remain line one to work.
    if lines and lines[0].startswith("#!"):
        lines.insert(1, header)
    else:
        lines.insert(0, header)

    path.write_text("".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fix", action="store_true", help="insert the missing header")
    parser.add_argument("--quiet", action="store_true", help="print nothing when clean")
    args = parser.parse_args()

    missing = [p for p in candidate_files() if not has_identifier(p)]
    total = len(candidate_files())

    if not missing:
        if not args.quiet:
            print(f"SPDX headers present: {total} file(s).")
        return 0

    if args.fix:
        for path in missing:
            insert_identifier(path)
            print(f"added SPDX header: {path.relative_to(REPO_ROOT)}")
        return 0

    print(f"Missing '{IDENTIFIER}':\n", file=sys.stderr)
    for path in missing:
        print(f"  {path.relative_to(REPO_ROOT)}", file=sys.stderr)
    print(f"\n{len(missing)} of {total} file(s). Run with --fix to insert.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
