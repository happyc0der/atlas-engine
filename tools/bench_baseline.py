#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Record and compare benchmark baselines.

A benchmark number is meaningless without the machine, compiler and build type that produced
it, so a baseline is stored per machine and a comparison refuses to run across machines
rather than quietly reporting a difference that is really a change of hardware.

    tools/bench_baseline.py record results.json              store as this machine's baseline
    tools/bench_baseline.py record results.json --only audio  update only those, keep the rest
    tools/bench_baseline.py compare results.json             compare against it
    tools/bench_baseline.py across a.json b.json             two machines, side by side

A regression flags a review. It is never a reason to edit the benchmark; see
docs/PERFORMANCE.md.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BASELINE_DIR = REPO_ROOT / "benchmarks" / "baselines"

# How much slower than the baseline counts as a regression.
#
# Measured rather than guessed: four consecutive runs of every scenario on the development
# machine spread by 1.03x at the median and never by more than 1.15x, excluding the ones noted
# below. A threshold of 1.25x therefore sits well clear of this machine's noise while still
# catching anything that matters, and a threshold that cries wolf is one nobody reads.
REGRESSION_FACTOR = 1.25
IMPROVEMENT_FACTOR = 0.80

# Below this, a ratio says nothing. The two scenarios whose run-to-run spread exceeded the
# threshold above both measure under two microseconds — one of them varied by 25x between runs
# while moving from nothing to nothing — because a ratio taken on a handful of timer ticks is
# a ratio of rounding. They are still reported, so a scenario that grows from nothing into
# something is visible, but they cannot fail a comparison.
MINIMUM_COMPARABLE_NS = 10_000

# Rows that measure the machine rather than the engine (benchmarks/src/bench_machine.cpp). They
# are never an engine regression. When they move, the machine moved — it was busy, throttled, or
# a different machine — and every other row moved with it for the same reason.
MACHINE_PREFIX = "machine/"


def per_unit_ns(result: dict) -> float | None:
    """Nanoseconds per unit of work, where a result says what its unit is."""
    units = result.get("units_per_iteration") or 0
    if result.get("metric") == "count" or units <= 0:
        return None
    return result["median_ns"] / units


def machine_id(environment: dict) -> str:
    """A filename-safe identifier for the machine and build a result came from."""
    parts = [
        environment.get("machine", "unknown"),
        environment.get("compiler", "unknown").split()[0],
        environment.get("build_type", "unknown"),
    ]
    joined = "-".join(parts).lower()
    return re.sub(r"[^a-z0-9.-]+", "-", joined).strip("-")


def load(path: Path) -> dict:
    if not path.is_file():
        sys.exit(f"error: {path} does not exist")
    return json.loads(path.read_text(encoding="utf-8"))


def working_tree_is_dirty() -> bool:
    """Whether the tree has uncommitted changes **now**.

    Asked here rather than trusted from the binary, because `environment["dirty"]` is captured
    at CMake *configure* time: edit a file, rebuild without reconfiguring, and the binary still
    reports a clean tree. That is correct for what it records — which commit the artifact was
    built from — and useless as a guard at the moment a baseline is written, which is later and
    is when the question actually matters.

    Found by overwriting this machine's baseline while expecting the stale flag to refuse.
    """
    try:
        status = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=no"],
            cwd=REPO_ROOT, capture_output=True, text=True, check=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        # No git, or it failed. Say nothing rather than refuse: the build_info flag below is
        # still checked, and a machine without git can still compare.
        return False
    return status.stdout.strip() != ""


def check_comparable(environment: dict, action: str) -> None:
    """Refuse to record or compare a result that cannot mean anything."""
    problems = []
    if environment.get("build_type") not in ("Release", "RelWithDebInfo"):
        problems.append(
            f"build type is {environment.get('build_type')}; an unoptimised build cannot be "
            f"compared with an optimised one"
        )
    if environment.get("sanitizer"):
        problems.append("a sanitizer was enabled, so the numbers measure the sanitizer")
    if environment.get("dirty"):
        problems.append(
            "the working tree had uncommitted changes when this was built, so the result is "
            "not reproducible from the recorded commit"
        )
    elif action.startswith("record") and working_tree_is_dirty():
        problems.append(
            "the working tree has uncommitted changes now. The binary was built from a clean "
            "one, so it does not know; see working_tree_is_dirty()"
        )

    if problems:
        print(f"refusing to {action}:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        sys.exit(1)


def record(results_path: Path, only: list[str] | None = None,
           replace: bool = False) -> int:
    """Store results as this machine's baseline, all of them or a named subset.

    **`--only` exists because recording used to be all or nothing, and that is what kept three
    milestones' benchmarks unrecorded.** M12 measured the mixer on a machine whose graphics
    scenarios were inflated up to 2.7x by a browser holding the device, and recording would have
    written those inflated numbers over good ones — so it recorded nothing, and audio has had no
    baseline since. Net and script then arrived with none either.

    A subset is not a workaround for a noisy machine. It is the honest shape of the problem: a
    scenario that is stable under load — the mixer is float arithmetic over a buffer and touches
    no device — can be recorded from a run in which another scenario was not stable, and saying
    so per scenario is more truthful than one file-level claim about the whole run.
    """
    data = load(results_path)
    check_comparable(data["environment"], "record a baseline")

    BASELINE_DIR.mkdir(parents=True, exist_ok=True)
    target = BASELINE_DIR / f"{machine_id(data['environment'])}.json"

    chosen = data["results"]
    if only:
        chosen = [r for r in data["results"]
                  if any(r["name"].startswith(prefix) for prefix in only)]
        if not chosen:
            names = sorted({r["name"].split("/")[0] for r in data["results"]})
            sys.exit(f"error: --only {' '.join(only)} matched nothing. "
                     f"This run has: {', '.join(names)}")

    if only and target.is_file():
        existing = load(target)
        # The environment must agree about everything except which commit produced it. A
        # baseline mixing two compilers or two build types would compare numbers that were
        # never comparable, which is the one thing this whole file exists to prevent.
        for field in ("machine", "cpu", "compiler", "build_type"):
            if existing["environment"].get(field) != data["environment"].get(field):
                sys.exit(f"error: refusing to merge into {target.name}: it was recorded with "
                         f"{field} {existing['environment'].get(field)!r} and this run has "
                         f"{data['environment'].get(field)!r}")

        # Each result carries the commit it came from, because a merged file has more than one
        # and a single environment block can no longer speak for all of them.
        merged = {(r["name"], r["parameters"]): r for r in existing["results"]}
        for result in chosen:
            result = dict(result)
            result["commit"] = data["environment"].get("commit")
            merged[(result["name"], result["parameters"])] = result
        combined = existing
        combined["results"] = [merged[key] for key in sorted(merged)]
        # The file-level environment keeps the commit of whatever was written last, and every
        # result that came from somewhere else now says so itself.
        combined["environment"] = data["environment"]
        target.write_text(json.dumps(combined, indent=2) + "\n", encoding="utf-8")
        print(f"updated {len(chosen)} result(s) in {target.relative_to(REPO_ROOT)}, "
              f"leaving {len(combined['results']) - len(chosen)} untouched")
    else:
        # Recording everything replaces the file. If that would drop scenarios the stored
        # baseline has — because this run was filtered, or a benchmark was not built — say so
        # and stop, rather than quietly shrinking the baseline to whatever happened to run.
        #
        # This check exists because its absence cost the baseline: `--filter audio` produced
        # four results, `record` wrote four results over seventy-two, and the only reason it was
        # recoverable is that the file was committed.
        if target.is_file() and not replace:
            stored = {(r["name"], r["parameters"]) for r in load(target)["results"]}
            arriving = {(r["name"], r["parameters"]) for r in chosen}
            lost = stored - arriving
            if lost:
                groups = sorted({name.split("/")[0] for name, _ in lost})
                sys.exit(
                    f"error: this would drop {len(lost)} stored result(s) from "
                    f"{target.name}, in: {', '.join(groups)}.\n"
                    f"  This run has {len(chosen)}; the baseline has {len(stored)}.\n"
                    f"  To update only what this run measured:  --only {' '.join(groups[:1])} …\n"
                    f"  To replace the baseline deliberately:   --replace")

        for result in chosen:
            result["commit"] = data["environment"].get("commit")
        data["results"] = chosen
        target.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        print(f"recorded {len(chosen)} results as {target.relative_to(REPO_ROOT)}")

    for result in chosen:
        print(f"  {result['name']} [{result['parameters']}]: "
              f"{result['median_ns'] / 1e6:.3f}ms")
    return 0


def compare(results_path: Path) -> int:
    data = load(results_path)
    identifier = machine_id(data["environment"])
    baseline_path = BASELINE_DIR / f"{identifier}.json"

    if not baseline_path.is_file():
        print(f"no baseline for this machine and build ({identifier}).")
        print(f"record one with: tools/bench_baseline.py record {results_path}")
        return 0

    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    previous = {
        (entry["name"], entry["parameters"]): entry for entry in baseline["results"]
    }

    print(f"comparing against {baseline_path.relative_to(REPO_ROOT)}")
    print(f"  baseline commit {baseline['environment'].get('commit')}, "
          f"current {data['environment'].get('commit')}\n")

    regressions = 0
    slower_machine = []
    for result in data["results"]:
        key = (result["name"], result["parameters"])
        if key not in previous:
            print(f"  NEW        {result['name']} [{result['parameters']}]: "
                  f"{result['median_ns'] / 1e6:.3f}ms")
            continue

        before = previous[key]["median_ns"]
        after = result["median_ns"]
        if before == 0:
            continue

        ratio = after / before
        machine_row = result["name"].startswith(MACHINE_PREFIX)
        label = "same"
        if before < MINIMUM_COMPARABLE_NS or after < MINIMUM_COMPARABLE_NS:
            label = "too small"
        elif ratio >= REGRESSION_FACTOR:
            if machine_row:
                label = "MACHINE"
                slower_machine.append(f"{result['name']} [{result['parameters']}] {ratio:.2f}x")
            else:
                label = "SLOWER"
                regressions += 1
        elif ratio <= IMPROVEMENT_FACTOR:
            label = "faster"

        print(f"  {label:<10} {result['name']} [{result['parameters']}]: "
              f"{before / 1e6:.3f}ms -> {after / 1e6:.3f}ms ({ratio:.2f}x)")

    if slower_machine:
        # Said before the regressions, because it changes how they read.
        print("\nthe machine itself measured slower than when the baseline was recorded:")
        for line in slower_machine:
            print(f"  {line}")
        print("so the rows marked SLOWER may be the machine's, not the engine's. Measure again on "
              "a quiet machine before believing them.")

    if regressions:
        print(f"\n{regressions} benchmark(s) regressed by more than "
              f"{(REGRESSION_FACTOR - 1) * 100:.0f} percent.")
        print("This is a prompt to look at why, not to adjust the benchmark.")
        return 1

    print("\nno regressions")
    return 0


def describe(label: str, environment: dict) -> None:
    print(f"{label}: {environment.get('machine')} ({environment.get('cpu')}, "
          f"{environment.get('cores')}, memory {environment.get('memory', 'not recorded')})")
    print(f"   {environment.get('os')}, {environment.get('compiler')} "
          f"{environment.get('build_type')}, commit {environment.get('commit')}"
          f"{' (dirty)' if environment.get('dirty') else ''}")


def across(first_path: Path, second_path: Path) -> int:
    """Two result files, usually from two machines, side by side: the machine rows first.

    `compare` refuses to cross machines, because a ratio between two machines is not a regression
    and must never fail anything. This prints one and fails nothing, and it prints the machine's
    own rows first, because M28 read a slow machine as a slow engine until a loop with no engine
    in it said otherwise: an engine row is read against the machine rows beside it.
    """
    first, second = load(first_path), load(second_path)
    for path, data in ((first_path, first), (second_path, second)):
        environment = data["environment"]
        if environment.get("build_type") not in ("Release", "RelWithDebInfo"):
            sys.exit(f"error: {path} is a {environment.get('build_type')} build; an unoptimised "
                     f"build says nothing about a machine")
        if environment.get("sanitizer"):
            sys.exit(f"error: {path} was built with a sanitizer, so it measures the sanitizer")

    describe("A", first["environment"])
    describe("B", second["environment"])

    rows_a = {(r["name"], r["parameters"]): r for r in first["results"]}
    rows_b = {(r["name"], r["parameters"]): r for r in second["results"]}
    shared = sorted(set(rows_a) & set(rows_b))
    machine = [key for key in shared if key[0].startswith(MACHINE_PREFIX)]
    engine = [key for key in shared if not key[0].startswith(MACHINE_PREFIX)]

    def show(keys: list) -> None:
        for key in keys:
            a, b = rows_a[key], rows_b[key]
            if a.get("metric") == "count":
                print(f"  {key[0]} [{key[1]}]: {a['median_ns']} -> {b['median_ns']}")
                continue
            ratio = b["median_ns"] / a["median_ns"] if a["median_ns"] else float("inf")
            unit_a, unit_b = per_unit_ns(a), per_unit_ns(b)
            per_unit = (f", {unit_a:.2f} -> {unit_b:.2f} ns a {a.get('unit_name', 'unit')[:-1]}"
                        if unit_a is not None and unit_b is not None else "")
            print(f"  {ratio:6.2f}x  {key[0]} [{key[1]}]: {a['median_ns'] / 1e6:.3f}ms -> "
                  f"{b['median_ns'] / 1e6:.3f}ms{per_unit}")

    print("\nThe machines (B against A):")
    if machine:
        show(machine)
    else:
        print("  neither file has both sets of machine rows. Without them this comparison cannot "
              "tell a slow machine from a slow engine; run atlas_bench --filter machine on both.")
    print("\nThe engine (B against A):")
    show(engine)
    only = (len(rows_a) - len(shared), len(rows_b) - len(shared))
    if any(only):
        print(f"\n{only[0]} row(s) only in A and {only[1]} only in B, not shown.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=["record", "compare", "across"])
    parser.add_argument("results", type=Path, help="JSON written by atlas_bench --json")
    parser.add_argument("other", type=Path, nargs="?",
                        help="for across: the second machine's JSON")
    parser.add_argument("--only", nargs="+", metavar="PREFIX",
                        help="record only results whose name starts with one of these, leaving "
                             "every other stored result untouched")
    parser.add_argument("--replace", action="store_true",
                        help="replace the whole baseline even when this run measured fewer "
                             "scenarios than it holds")
    args = parser.parse_args()

    if args.action != "record" and (args.only or args.replace):
        sys.exit("error: --only and --replace apply to record")
    if (args.action == "across") != (args.other is not None):
        sys.exit("error: across takes two result files, and only across does")
    if args.action == "across":
        return across(args.results, args.other)
    return (record(args.results, args.only, args.replace) if args.action == "record"
            else compare(args.results))


if __name__ == "__main__":
    sys.exit(main())
