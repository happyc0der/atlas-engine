#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that every record of the pinned vcpkg commit names the same commit.

The pin is written down in three places, and none of them can be removed:

  - the submodule's gitlink, which is what `git submodule update` checks out;
  - `builtin-baseline` in `vcpkg.json`, which is what vcpkg resolves versions against;
  - the vcpkg row of `docs/DEPENDENCIES.md`, which is what a person reads.

The continuous-integration binary cache is keyed on a hash of `vcpkg.json`, so it follows the
baseline and not the gitlink. Until M23 that was a recorded gap: bump the submodule without
bumping the baseline, and every lane would restore a cache built against a different port tree
with nothing to say so. Keying the cache on the gitlink as well would have narrowed that. This
closes it, by making the two records unable to disagree — a bump that moves one and not the
other fails here, in precheck and in the lint workflow, before any cache is consulted.

The gitlink is read from the index rather than from HEAD, so a bump that is staged and not yet
committed is judged too.

Exit code 0 when all three agree, 1 otherwise.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SUBMODULE = "external/vcpkg"
MANIFEST = REPO_ROOT / "vcpkg.json"
DEPENDENCIES = REPO_ROOT / "docs" / "DEPENDENCIES.md"
SHA = re.compile(r"^[0-9a-f]{40}$")


def gitlink() -> str:
    """The commit the index records for the submodule."""
    listing = subprocess.run(["git", "ls-files", "--stage", "--", SUBMODULE], cwd=REPO_ROOT,
                             capture_output=True, text=True, check=True).stdout.split()
    # "<mode> <object> <stage>\t<path>": a submodule's mode is 160000 and its object a commit.
    if len(listing) < 4 or listing[0] != "160000":
        raise SystemExit(f"{SUBMODULE} is not a submodule in the index: {' '.join(listing)!r}")
    return listing[1]


def baseline() -> str:
    return json.loads(MANIFEST.read_text(encoding="utf-8"))["builtin-baseline"]


def documented() -> str:
    for line in DEPENDENCIES.read_text(encoding="utf-8").splitlines():
        if line.startswith("| vcpkg |"):
            match = re.search(r"commit `([0-9a-f]{40})`", line)
            if match:
                return match.group(1)
            raise SystemExit(f"the vcpkg row of {DEPENDENCIES.name} names no commit: {line}")
    raise SystemExit(f"{DEPENDENCIES.name} has no vcpkg row")


def main() -> int:
    records = {
        f"the gitlink of {SUBMODULE}": gitlink(),
        "builtin-baseline in vcpkg.json": baseline(),
        f"the vcpkg row of docs/{DEPENDENCIES.name}": documented(),
    }
    for where, commit in records.items():
        if not SHA.match(commit):
            print(f"vcpkg pin: {where} is {commit!r}, which is not a full commit", file=sys.stderr)
            return 1
    if len(set(records.values())) != 1:
        print("vcpkg pin: the records disagree, so the binary cache could be keyed on one pin "
              "and built against another:", file=sys.stderr)
        for where, commit in records.items():
            print(f"  {commit}  {where}", file=sys.stderr)
        print("bump all three together", file=sys.stderr)
        return 1
    print(f"vcpkg pin: all three records name {records[f'the gitlink of {SUBMODULE}']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
