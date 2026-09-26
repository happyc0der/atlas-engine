#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a project that is not Atlas against an installed Atlas, and run it (ADR-0024 D10).

Registered with CTest under the label `package`, in two parts:

    run.py --build-dir B --install
        The fixture. Installs the Atlas build B into B/package/prefix with tools/sdk.py, once,
        before any case runs.

    run.py --build-dir B --case NAME [how to break it] [--expect STAGE --expect-message REGEX]
        One case. Configures tests/package/consumer against the prefix and the vcpkg tree beside
        it, with no vcpkg toolchain and no package registry; builds it; checks its include paths;
        runs it.

The stages, in order: `configure`, `build`, `guard`, `run`. The guard fails if any include path
the consumer compiled with lies in the Atlas source tree outside the prefix, the vcpkg tree and
the consumer's own directories, and, with MSVC, if the consumer's language standard flag is not
the one Atlas was compiled with.

A case that breaks something on purpose names the stage it must fail at and a pattern the
failure must print. It passes only if exactly that happens: failing earlier, failing for another
reason, or not failing are each a failure, so the case cannot pass by accident the way a bare
WILL_FAIL would.

The prefix is shared by every case. A case that must break the prefix itself works on a copy made
of hard links, and unlinks a file before changing it, so the shared prefix is never touched.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONSUMER = ROOT / "tests" / "package" / "consumer"
STAGES = ("configure", "build", "guard", "run")


class StageFailed(Exception):
    def __init__(self, stage: str, output: str) -> None:
        super().__init__(stage)
        self.stage = stage
        self.output = output


def read_cache(build_dir: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in (build_dir / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        match = re.match(r"^([A-Za-z_][A-Za-z0-9_]*):[A-Z_]+=(.*)$", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def compiler_id(build_dir: Path) -> str:
    """The compiler CMake identified for the Atlas build, which the cache does not record."""
    for path in sorted(build_dir.glob("CMakeFiles/*/CMakeCXXCompiler.cmake")):
        match = re.search(r'set\(CMAKE_CXX_COMPILER_ID "([^"]*)"\)', path.read_text(encoding="utf-8"))
        if match:
            return match.group(1)
    return ""


def clean_environment() -> dict[str, str]:
    """The consumer sees none of the variables that would lead it to Atlas's own configuration."""
    env = dict(os.environ)
    for name in ("CMAKE_TOOLCHAIN_FILE", "CMAKE_PREFIX_PATH", "VCPKG_ROOT", "VCPKG_INSTALLATION_ROOT"):
        env.pop(name, None)
    return env


def run(stage: str, command: list[str], cwd: Path | None = None) -> str:
    result = subprocess.run(command, cwd=cwd, env=clean_environment(), capture_output=True,
                            text=True, check=False)
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise StageFailed(stage, f"$ {' '.join(command)}\n{output}")
    return output


def paths(build_dir: Path) -> tuple[Path, Path, Path]:
    package = build_dir / "package"
    return package, package / "prefix", package / "deps.txt"


def install(build_dir: Path) -> int:
    package, prefix, deps_file = paths(build_dir)
    if prefix.exists():
        shutil.rmtree(prefix)
    package.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([sys.executable, str(ROOT / "tools" / "sdk.py"), "--build-dir",
                             str(build_dir), "--prefix", str(prefix)],
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        print(result.stdout + result.stderr)
        return 1
    lines = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
    deps_file.write_text(lines["ATLAS_DEPS"], encoding="utf-8")
    print(f"installed {prefix}\nvcpkg tree {lines['ATLAS_DEPS']}")
    return 0


def linked_copy(source: Path, destination: Path) -> None:
    """The prefix again, as hard links: cheap, and a file is unlinked before it is changed."""
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(source, destination, copy_function=os.link)


def public_headers() -> list[str]:
    """Every header the source tree says is public: what the install should contain."""
    roots = sorted(ROOT.glob("engine/*/include")) + [ROOT / "apps" / "common" / "include"]
    headers = []
    for root in roots:
        for path in sorted(root.rglob("*")):
            if path.suffix in (".hpp", ".h"):
                headers.append(path.relative_to(root).as_posix())
    return headers


INCLUDE_FLAG = re.compile(r"^(?:-I|-isystem|-iquote|/I|-external:I|/external:I)(.*)$")


def include_paths(entry: dict) -> list[str]:
    arguments = entry.get("arguments")
    if arguments is None:
        arguments = shlex.split(entry["command"], posix=os.name != "nt")
    found = []
    pending = False
    for argument in arguments:
        argument = argument.strip('"')
        if pending:
            found.append(argument)
            pending = False
            continue
        match = INCLUDE_FLAG.match(argument)
        if match:
            if match.group(1):
                found.append(match.group(1))
            else:
                pending = True
    return found


def standard_flag(command: str) -> str:
    """The last language standard flag on a command line, which is the one the compiler obeys."""
    flags = re.findall(r"(?:^|\s)[-/]std:(\S+)", command)
    return flags[-1] if flags else ""


def inside(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def guard(build_dir: Path, consumer_build: Path, prefix: Path, deps: Path, compiler: str) -> str:
    source = ROOT.resolve()
    allowed = [prefix.resolve(), deps.resolve(), CONSUMER.resolve(), consumer_build.resolve()]
    entries = json.loads((consumer_build / "compile_commands.json").read_text(encoding="utf-8"))
    report = []
    for entry in entries:
        for raw in include_paths(entry):
            path = (Path(entry["directory"]) / raw).resolve()
            if inside(path, source) and not any(inside(path, root) for root in allowed):
                raise StageFailed("guard", f"{entry['file']} is compiled with an include path in "
                                           f"the Atlas source tree: {path}")
    report.append(f"guard: {len(entries)} compile commands, every include path in the prefix, "
                  f"the vcpkg tree or the consumer")

    if compiler == "MSVC":
        atlas_entries = json.loads((build_dir / "compile_commands.json").read_text(encoding="utf-8"))
        atlas_flag = next(standard_flag(e["command"]) for e in atlas_entries
                          if e["file"].replace("\\", "/").endswith("engine/core/src/error.cpp"))
        for entry in entries:
            flag = standard_flag(entry.get("command") or " ".join(entry["arguments"]))
            if flag != atlas_flag:
                raise StageFailed("guard", f"{entry['file']} is compiled with /std:{flag} and Atlas "
                                           f"with /std:{atlas_flag}")
        report.append(f"guard: every compile uses /std:{atlas_flag}, as Atlas does")
    return "\n".join(report)


def build_case(args: argparse.Namespace) -> str:
    build_dir = Path(args.build_dir).resolve()
    cache = read_cache(build_dir)
    package, shared_prefix, deps_file = paths(build_dir)
    if not deps_file.is_file():
        raise SystemExit("error: the package fixture has not run; run the case through CTest")
    deps = Path(deps_file.read_text(encoding="utf-8").strip())
    work = package / args.case
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    prefix = shared_prefix
    if args.remove_from_prefix or args.drop_dependency:
        prefix = work / "prefix"
        linked_copy(shared_prefix, prefix)
        for relative in args.remove_from_prefix or []:
            (prefix / relative).unlink()
        if args.drop_dependency:
            config = prefix / "lib" / "cmake" / "Atlas" / "AtlasConfig.cmake"
            text = config.read_text(encoding="utf-8")
            config.unlink()
            config.write_text(text.replace(f" {args.drop_dependency} ", " "), encoding="utf-8")

    headers = work / "headers.txt"
    headers.write_text("\n".join(public_headers()) + "\n", encoding="utf-8")

    consumer_build = work / "build"
    configure = ["cmake", "-S", str(CONSUMER), "-B", str(consumer_build),
                 "-G", cache["CMAKE_GENERATOR"],
                 f"-DCMAKE_CXX_COMPILER={cache['CMAKE_CXX_COMPILER']}",
                 f"-DCMAKE_BUILD_TYPE={cache.get('CMAKE_BUILD_TYPE', '')}",
                 f"-DCMAKE_PREFIX_PATH={prefix};{deps}",
                 "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
                 "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                 f"-DATLAS_CONSUMER_HEADERS={headers}",
                 f"-DATLAS_CONSUMER_MUTATION={args.mutation or ''}",
                 f"-DATLAS_CONSUMER_SOURCE_INCLUDE={ROOT / 'engine' / 'core' / 'include'}"]
    for name in ("CMAKE_MAKE_PROGRAM", "CMAKE_OSX_DEPLOYMENT_TARGET"):
        if cache.get(name):
            configure.append(f"-D{name}={cache[name]}")
    output = [run("configure", configure)]

    command = ["cmake", "--build", str(consumer_build)]
    if args.target:
        command += ["--target", args.target]
    output.append(run("build", command))

    output.append(guard(build_dir, consumer_build, prefix, deps, compiler_id(build_dir)))

    if args.target in (None, "atlas_consumer"):
        executable = consumer_build / ("atlas_consumer.exe" if os.name == "nt" else "atlas_consumer")
        output.append(run("run", [str(executable)]))
    return "\n".join(output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", required=True, help="the Atlas build to install and consume")
    parser.add_argument("--install", action="store_true", help="the fixture: install, and stop")
    parser.add_argument("--case", help="this case's name, which is also its working directory")
    parser.add_argument("--mutation", help="ATLAS_CONSUMER_MUTATION for the consumer")
    parser.add_argument("--target", help="build only this target of the consumer")
    parser.add_argument("--remove-from-prefix", action="append", metavar="PATH",
                        help="delete this file from a copy of the prefix")
    parser.add_argument("--drop-dependency", metavar="PACKAGE",
                        help="remove this package from a copy of the config's dependency lists")
    parser.add_argument("--expect", choices=STAGES, help="the stage this case must fail at")
    parser.add_argument("--expect-message", help="a pattern the expected failure must print")
    args = parser.parse_args()

    if args.install:
        return install(Path(args.build_dir).resolve())
    if not args.case:
        parser.error("name a --case, or pass --install")
    if bool(args.expect) != bool(args.expect_message):
        parser.error("--expect and --expect-message go together")

    try:
        output = build_case(args)
    except StageFailed as failure:
        if not args.expect:
            print(f"FAILED at {failure.stage}:\n{failure.output}")
            return 1
        if failure.stage != args.expect:
            print(f"expected a failure at {args.expect}, got one at {failure.stage}:\n"
                  f"{failure.output}")
            return 1
        if not re.search(args.expect_message, failure.output):
            print(f"failed at {failure.stage} as expected, but not saying "
                  f"/{args.expect_message}/:\n{failure.output}")
            return 1
        print(f"failed at {failure.stage}, as expected, saying /{args.expect_message}/")
        return 0

    if args.expect:
        print(f"expected a failure at {args.expect}, and every stage passed:\n{output}")
        return 1
    print(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
