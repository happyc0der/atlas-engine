#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Record and compare benchmark baselines.

A benchmark number is meaningless without the machine, compiler and build type that produced
it, so a baseline is stored per machine and a comparison refuses to run across machines
rather than quietly reporting a difference that is really a change of hardware.

    tools/bench_baseline.py record results.json     store as this machine's baseline
    tools/bench_baseline.py compare results.json    compare against it

A regression flags a review. It is never a reason to edit the benchmark; see
docs/PERFORMANCE.md.
"""

from __future__ import annotations

import argparse
import json
import re
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
            "the working tree had uncommitted changes, so the result is not reproducible "
            "from the recorded commit"
        )

    if problems:
        print(f"refusing to {action}:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        sys.exit(1)


def record(results_path: Path) -> int:
    data = load(results_path)
    check_comparable(data["environment"], "record a baseline")

    BASELINE_DIR.mkdir(parents=True, exist_ok=True)
    target = BASELINE_DIR / f"{machine_id(data['environment'])}.json"
    target.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")

    print(f"recorded {len(data['results'])} results as {target.relative_to(REPO_ROOT)}")
    for result in data["results"]:
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
        label = "same"
        if before < MINIMUM_COMPARABLE_NS or after < MINIMUM_COMPARABLE_NS:
            label = "too small"
        elif ratio >= REGRESSION_FACTOR:
            label = "SLOWER"
            regressions += 1
        elif ratio <= IMPROVEMENT_FACTOR:
            label = "faster"

        print(f"  {label:<10} {result['name']} [{result['parameters']}]: "
              f"{before / 1e6:.3f}ms -> {after / 1e6:.3f}ms ({ratio:.2f}x)")

    if regressions:
        print(f"\n{regressions} benchmark(s) regressed by more than "
              f"{(REGRESSION_FACTOR - 1) * 100:.0f} percent.")
        print("This is a prompt to look at why, not to adjust the benchmark.")
        return 1

    print("\nno regressions")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=["record", "compare"])
    parser.add_argument("results", type=Path, help="JSON written by atlas_bench --json")
    args = parser.parse_args()

    return record(args.results) if args.action == "record" else compare(args.results)


if __name__ == "__main__":
    sys.exit(main())
