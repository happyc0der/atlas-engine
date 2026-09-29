#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hold what Atlas installs and the version it is installed under together (ADR-0024 D1).

The minor version is the milestone that last changed what is installed, in name or in meaning.
M28 and M30 changed it — a new constant, what `display_scale()` returns, what the overlay does
with a pointer — and both left 0.27, so atlas-chess, pinned at M27, could not tell that anything
it used had moved. This records a digest of everything installed beside the version it was
recorded under, and fails when any of it changes while the version stays:

  - every public header, installed by directory from engine/<module>/include and
    apps/common/include;
  - the runtime data in share/atlas: the sprite shaders, en.json and build_mods.py;
  - cmake/AtlasConfig.cmake.in, from which the package's config is generated;
  - the names of the exported targets: every module in cmake/ModuleGraph.cmake but the internal
    ones and `runtime`, atlas::sdl3, and the app kit's three.

**Bytes, comments included, on purpose.** M30 reached the installed tree only as comments, and
M28's change of meaning only as a comment beside a new constant: a guard that ignored comments
would have missed M30 entirely. `.gitattributes` pins LF in every working tree, so a digest is the
same on every platform.

When it fails there are two ways out, and both are decisions:

    python3 tools/check_installed_api.py --record
        after moving the minor in CMakeLists.txt and everywhere the version is named;

    python3 tools/check_installed_api.py --record --same-version "<why no meaning changed>"
        for a change that leaves every meaning alone: a typo, or a second change in a milestone
        that has already moved the minor. The reason is kept in the record.

It also fails when a place that names the version disagrees with CMakeLists.txt.

**What it cannot see.** A meaning that changes in a .cpp alone, as M31's did when a quiet peer
came to mean one the transport has stopped hearing; the libraries the SDK carries beside the
install (vcpkg.json), as M28's SDL with Vulkan was; and the usage requirements the generated
target files carry, such as an exported compile option. Those remain decisions made by hand.

Two more uses:

    python3 tools/check_installed_api.py --compare OLD [NEW]
        what the installed surface changed between two revisions, NEW defaulting to the working
        tree: what a consumer moving its pin from OLD takes on. Informational; exits 0.

    python3 tools/check_installed_api.py --against-prefix PREFIX
        a real install: its include/ and share/atlas/ hold exactly the files computed here, with
        the same bytes, and its target files declare exactly these targets. The `package` label
        runs it, so the rules below cannot drift from cmake/AtlasInstall.cmake.

Exit code 0 when everything agrees, 1 otherwise.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RECORD = ROOT / "tools" / "installed_api.json"
RECORD_FORMAT = "atlas-installed-api"
RECORD_FORMAT_VERSION = 1

# Where each installed data file comes from, exactly as cmake/AtlasInstall.cmake installs it.
# `--against-prefix` is what keeps this honest.
DATA = {
    f"share/atlas/shaders/sprite.{stage}.{format_}":
        f"assets/cooked/shaders/sprite.{stage}.{format_}"
    for stage in ("vert", "frag") for format_ in ("spv", "msl")
} | {
    "share/atlas/strings/en.json": "assets/source/strings/en.json",
    "share/atlas/tools/build_mods.py": "tools/build_mods.py",
}
CONFIG_TEMPLATE = "cmake/AtlasConfig.cmake.in"
APP_KIT = ("app_common", "app_lockstep", "app_mods")
HEADER_SUFFIXES = (".hpp", ".h")


def git(*args: str) -> bytes:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, check=True).stdout


class Tree:
    """The source tree as it is on disk, or as it was at a revision."""

    def __init__(self, revision: str | None = None) -> None:
        self.revision = revision
        self._listed: list[str] = []
        if revision:
            self._listed = git("ls-tree", "-r", "--name-only", revision).decode().splitlines()

    def files_under(self, directory: str) -> list[str]:
        if self.revision:
            prefix = directory.rstrip("/") + "/"
            return sorted(path for path in self._listed if path.startswith(prefix))
        base = ROOT / directory
        return sorted(path.relative_to(ROOT).as_posix()
                      for path in base.rglob("*") if path.is_file()) if base.is_dir() else []

    def has_directory(self, directory: str) -> bool:
        return bool(self.files_under(directory))

    def read(self, path: str) -> bytes:
        if self.revision:
            return git("show", f"{self.revision}:{path}")
        return (ROOT / path).read_bytes()


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def installed_modules(tree: Tree) -> list[str]:
    """The modules cmake/AtlasInstall.cmake installs: all but the internal ones, and `runtime`
    while it is listed in the graph before it exists."""
    text = tree.read("cmake/ModuleGraph.cmake").decode()
    match = re.search(r"set\(ATLAS_MODULES\s+(.*?)\s+CACHE", text, re.DOTALL)
    if not match:
        raise SystemExit("installed API: cannot find ATLAS_MODULES in cmake/ModuleGraph.cmake")
    modules = []
    for module in match.group(1).split():
        if module.endswith("_internal"):
            continue
        if module == "runtime" and not tree.has_directory("engine/runtime"):
            continue
        modules.append(module)
    return modules


@dataclass
class Surface:
    """Everything a consumer of the install is given: files by installed path, and targets."""

    files: dict[str, str] = field(default_factory=dict)
    config_template: str = ""
    targets: list[str] = field(default_factory=list)
    sources: dict[str, str] = field(default_factory=dict)


def header_roots(tree: Tree) -> list[str]:
    return [f"engine/{module}/include" for module in installed_modules(tree)] + [
        "apps/common/include"]


def surface(tree: Tree) -> Surface:
    result = Surface()
    for root in header_roots(tree):
        for source in tree.files_under(root):
            if source.endswith(HEADER_SUFFIXES):
                installed = "include/" + source[len(root) + 1:]
                if installed in result.files:
                    raise SystemExit(f"installed API: two sources install {installed}: "
                                     f"{result.sources[installed]} and {source}")
                result.files[installed] = digest(tree.read(source))
                result.sources[installed] = source
    for installed, source in DATA.items():
        result.files[installed] = digest(tree.read(source))
        result.sources[installed] = source
    result.files = dict(sorted(result.files.items()))
    result.config_template = digest(tree.read(CONFIG_TEMPLATE))
    result.targets = sorted(["atlas::sdl3"] + [f"atlas::{m}" for m in installed_modules(tree)]
                            + [f"atlas::{t}" for t in APP_KIT])
    return result


def public_headers() -> list[str]:
    """Every public header, as a consumer includes it: what the install should contain."""
    return [path[len("include/"):] for path in surface(Tree()).files if path.startswith("include/")]


def differences(old: dict[str, str], new: dict[str, str]) -> list[tuple[str, str]]:
    changes = [("changed", path) for path in old.keys() & new.keys() if old[path] != new[path]]
    changes += [("appeared", path) for path in new.keys() - old.keys()]
    changes += [("gone", path) for path in old.keys() - new.keys()]
    return sorted(changes, key=lambda change: (change[1], change[0]))


def surface_differences(old: Surface, new: Surface) -> list[tuple[str, str]]:
    changes = differences(old.files, new.files)
    if old.config_template != new.config_template:
        changes.append(("changed", CONFIG_TEMPLATE))
    changes += [("appeared", target) for target in sorted(set(new.targets) - set(old.targets))]
    changes += [("gone", target) for target in sorted(set(old.targets) - set(new.targets))]
    return changes


def print_changes(changes: list[tuple[str, str]], stream=sys.stdout) -> None:
    for kind, what in changes:
        print(f"  {kind:<9}{what}", file=stream)


# The places that name the version besides CMakeLists.txt. Each pattern's group is the version
# it names. The package consumer's refusal case is the one place that must name an older one.
@dataclass(frozen=True)
class Site:
    path: str
    pattern: str
    full: bool = False  # names major.minor.patch rather than major.minor


SITES = (
    Site("CLAUDE.md", r"find_package\(Atlas (\d+\.\d+) COMPONENTS app\)"),
    Site("docs/ARCHITECTURE.md", r"find_package\(Atlas (\d+\.\d+)\s"),
    Site("tests/package/consumer/CMakeLists.txt", r"set\(_atlas_version (\d+\.\d+)\)\n"
                                                  r"if\(ATLAS_CONSUMER_MUTATION"),
    Site("engine/core/tests/test_build_info.cpp", r'info::version\(\) == "(\d+\.\d+\.\d+)"',
         full=True),
    Site("engine/core/tests/test_build_info.cpp", r'summary\.contains\("(\d+\.\d+\.\d+)"\)',
         full=True),
)
OLDER_REQUEST = (Site("tests/package/consumer/CMakeLists.txt",
                      r'MUTATION STREQUAL "old_version"\)\n\s*set\(_atlas_version (\d+\.\d+)\)'),)


def project_version() -> str:
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(Atlas\s+VERSION (\d+\.\d+\.\d+)", text)
    if not match:
        raise SystemExit("installed API: CMakeLists.txt names no `project(Atlas VERSION x.y.z)`")
    return match.group(1)


def minor_of(version: str) -> str:
    return ".".join(version.split(".")[:2])


def as_tuple(version: str) -> tuple[int, ...]:
    return tuple(int(part) for part in version.split("."))


def version_problems(version: str) -> list[str]:
    problems = []
    for site in SITES:
        text = (ROOT / site.path).read_text(encoding="utf-8")
        found = re.findall(site.pattern, text)
        wanted = version if site.full else minor_of(version)
        if not found:
            problems.append(f"{site.path} no longer names the version where this looks for it "
                            f"(/{site.pattern}/)")
        problems += [f"{site.path} names {named}, and CMakeLists.txt {version}"
                     for named in found if named != wanted]
    for site in OLDER_REQUEST:
        found = re.findall(site.pattern, (ROOT / site.path).read_text(encoding="utf-8"))
        if not found:
            problems.append(f"{site.path} has no refusal case where this looks for it")
        problems += [f"{site.path}'s refusal case asks for {named}, which is not older than "
                     f"{minor_of(version)}"
                     for named in found if as_tuple(named) >= as_tuple(minor_of(version))]
    return problems


def load_record() -> dict | None:
    if not RECORD.is_file():
        return None
    try:
        record = json.loads(RECORD.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise SystemExit(f"installed API: {RECORD.name} is not JSON: {error}") from error
    if (record.get("format") != RECORD_FORMAT
            or record.get("format_version") != RECORD_FORMAT_VERSION):
        raise SystemExit(f"installed API: {RECORD.name} is not a version "
                         f"{RECORD_FORMAT_VERSION} {RECORD_FORMAT} record")
    return record


def recorded_surface(record: dict) -> Surface:
    return Surface(files=record["files"], config_template=record["config_template"],
                   targets=record["targets"])


def write_record(minor: str, current: Surface, log: list[dict]) -> None:
    record = {
        "format": RECORD_FORMAT,
        "format_version": RECORD_FORMAT_VERSION,
        "atlas": minor,
        "targets": current.targets,
        "config_template": current.config_template,
        "files": current.files,
        "same_version": log,
    }
    RECORD.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


WAYS_OUT = """\
A consumer pinned to {minor} would take this on without being told. Either
  - move the minor in CMakeLists.txt and wherever the version is named, then run
      python3 tools/check_installed_api.py --record
  - or, if no installed meaning changed (a typo, or a second change in a milestone that has
    already moved the minor), say so:
      python3 tools/check_installed_api.py --record --same-version "<why>\""""


def check() -> int:
    version = project_version()
    minor = minor_of(version)
    problems = version_problems(version)
    for problem in problems:
        print(f"installed API: {problem}", file=sys.stderr)
    record = load_record()
    if record is None:
        print(f"installed API: no {RECORD.relative_to(ROOT)}; run "
              f"python3 tools/check_installed_api.py --record", file=sys.stderr)
        return 1
    current = surface(Tree())
    if record["atlas"] != minor:
        if as_tuple(minor) < as_tuple(record["atlas"]):
            print(f"installed API: the version went back from {record['atlas']} to {minor}",
                  file=sys.stderr)
        else:
            print(f"installed API: the version is {minor} and the record is for "
                  f"{record['atlas']}; run python3 tools/check_installed_api.py --record, so the "
                  f"next change under {minor} is caught", file=sys.stderr)
        return 1
    changes = surface_differences(recorded_surface(record), current)
    if changes:
        print(f"installed API: what is installed changed while the version stayed {minor} "
              f"({len(changes)} change(s)):", file=sys.stderr)
        print_changes(changes, sys.stderr)
        print(WAYS_OUT.format(minor=minor), file=sys.stderr)
        return 1
    if problems:
        return 1
    print(f"installed API: {len(current.files)} files, the config template and "
          f"{len(current.targets)} targets match the record for {minor}")
    return 0


def record_command(same_version: str | None) -> int:
    version = project_version()
    minor = minor_of(version)
    problems = version_problems(version)
    if problems:
        for problem in problems:
            print(f"installed API: {problem}", file=sys.stderr)
        print("installed API: not recorded; make every place name one version first",
              file=sys.stderr)
        return 1
    record = load_record()
    current = surface(Tree())
    log = record["same_version"] if record else []
    if record is not None and record["atlas"] == minor:
        changes = surface_differences(recorded_surface(record), current)
        if not changes:
            print(f"installed API: nothing changed under {minor}; the record stands")
            return 0
        if same_version is None:
            print(f"installed API: what is installed changed and the version is still {minor} "
                  f"({len(changes)} change(s)):", file=sys.stderr)
            print_changes(changes, sys.stderr)
            print(WAYS_OUT.format(minor=minor), file=sys.stderr)
            return 1
        if not same_version.strip():
            print("installed API: --same-version needs a reason a reader can check",
                  file=sys.stderr)
            return 1
        log = log + [{"atlas": minor, "reason": same_version.strip(),
                      "changed": [what for _, what in changes]}]
    elif same_version is not None:
        print(f"installed API: --same-version is for a change under the recorded version, and "
              f"the version moved to {minor}; record without it", file=sys.stderr)
        return 1
    elif record is not None and as_tuple(minor) < as_tuple(record["atlas"]):
        print(f"installed API: the version went back from {record['atlas']} to {minor}; "
              f"not recorded", file=sys.stderr)
        return 1
    write_record(minor, current, log)
    print(f"installed API: recorded {len(current.files)} files, the config template and "
          f"{len(current.targets)} targets under {minor}")
    return 0


def compare_command(old: str, new: str | None) -> int:
    changes = surface_differences(surface(Tree(old)), surface(Tree(new)))
    where = new or "the working tree"
    if not changes:
        print(f"installed API: nothing installed changed from {old} to {where}")
        return 0
    print(f"installed API: {len(changes)} change(s) to what is installed, from {old} to {where}:")
    print_changes(changes)
    return 0


def target_names(prefix: Path) -> list[str]:
    names = []
    for targets in sorted(prefix.glob("**/cmake/Atlas/Atlas*Targets.cmake")):
        names += re.findall(r"^add_library\((atlas::[A-Za-z0-9_]+) ",
                            targets.read_text(encoding="utf-8"), re.MULTILINE)
    return sorted(names)


def against_prefix_command(prefix: Path) -> int:
    expected = surface(Tree())
    found = {}
    for top in ("include", "share/atlas"):
        base = prefix / top
        if base.is_dir():
            for path in base.rglob("*"):
                if path.is_file():
                    found[path.relative_to(prefix).as_posix()] = digest(path.read_bytes())
    changes = differences(expected.files, found)
    targets = target_names(prefix)
    changes += [("appeared", t) for t in sorted(set(targets) - set(expected.targets))]
    changes += [("gone", t) for t in sorted(set(expected.targets) - set(targets))]
    if changes:
        print(f"installed API: the install at {prefix} is not what this tool computes; "
              f"cmake/AtlasInstall.cmake and the rules here have drifted apart:", file=sys.stderr)
        print_changes(changes, sys.stderr)
        return 1
    print(f"installed API: {prefix} holds exactly the {len(found)} files and "
          f"{len(targets)} targets computed here")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--record", action="store_true",
                        help="write the record for the current version")
    action.add_argument("--compare", nargs="+", metavar="REV",
                        help="list what the installed surface changed between OLD and NEW")
    action.add_argument("--against-prefix", type=Path, metavar="PREFIX",
                        help="check a real install against what this tool computes")
    parser.add_argument("--same-version", metavar="REASON",
                        help="with --record: accept a change under the same version, and why")
    args = parser.parse_args()
    if args.same_version is not None and not args.record:
        parser.error("--same-version goes with --record")

    if args.record:
        return record_command(args.same_version)
    if args.compare:
        if len(args.compare) > 2:
            parser.error("--compare takes OLD and, optionally, NEW")
        return compare_command(args.compare[0],
                               args.compare[1] if len(args.compare) > 1 else None)
    if args.against_prefix:
        return against_prefix_command(args.against_prefix.resolve())
    return check()


if __name__ == "__main__":
    sys.exit(main())
