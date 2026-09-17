#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end checks that run the sandbox binary.

The sandbox is the composition root: it is the only place where subsystem construction
order, shutdown order, exit codes and lifecycle logging are exercised together. Unit tests
cover the modules; nothing but this covers the wiring between them.

Each case is registered as its own CTest entry so a failure names the behaviour that broke
rather than "the integration test". Run one with:

    python3 tests/integration/sandbox_checks.py --binary <path> --case <name>
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import tempfile

# Long enough that a slow machine under a sanitizer is not mistaken for a hang, short enough
# that a real hang is reported rather than waited on.
TIMEOUT_SECONDS = 180


class CheckFailed(Exception):
    pass


def run(binary: str, args: list[str], cwd: str | None = None) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(
            [binary, *args],
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
            cwd=cwd,
            check=False,
        )
    except subprocess.TimeoutExpired as expired:
        raise CheckFailed(
            f"{' '.join(args)} did not finish within {TIMEOUT_SECONDS}s"
        ) from expired


def output_of(result: subprocess.CompletedProcess) -> str:
    return result.stdout + result.stderr


def expect_exit(result: subprocess.CompletedProcess, code: int, what: str) -> None:
    if result.returncode != code:
        raise CheckFailed(
            f"{what}: expected exit {code}, got {result.returncode}\n"
            f"--- output ---\n{output_of(result)}"
        )


def expect_contains(text: str, needle: str, what: str) -> None:
    if needle not in text:
        raise CheckFailed(f"{what}: expected to find {needle!r}\n--- output ---\n{text}")


def expect_ordered(text: str, needles: list[str], what: str) -> None:
    """Assert the needles appear, in this order.

    Order is the point: a run that logs shutdown before running has torn itself down in the
    wrong sequence, and every needle would still be present.
    """
    position = 0
    for needle in needles:
        found = text.find(needle, position)
        if found < 0:
            later = text.find(needle)
            if later >= 0:
                raise CheckFailed(
                    f"{what}: found {needle!r} but out of order\n--- output ---\n{text}"
                )
            raise CheckFailed(f"{what}: never found {needle!r}\n--- output ---\n{text}")
        position = found + len(needle)


# --- cases -----------------------------------------------------------------------------


def case_version(binary: str) -> None:
    result = run(binary, ["--version"])
    expect_exit(result, 0, "--version")
    text = output_of(result)
    expect_contains(text, "Atlas", "--version")
    # The build identity is what makes a bug report actionable, so it has to be there.
    for part in ("Debug", "RelWithDebInfo", "Release"):
        if part in text:
            break
    else:
        raise CheckFailed(f"--version: no build type in output\n--- output ---\n{text}")


def case_help(binary: str) -> None:
    result = run(binary, ["--help"])
    expect_exit(result, 0, "--help")
    text = output_of(result)
    for flag in ("--headless", "--ticks", "--version"):
        expect_contains(text, flag, "--help")


def case_headless_lifecycle(binary: str) -> None:
    result = run(binary, ["--headless", "--ticks", "120"])
    expect_exit(result, 0, "a bounded headless run")
    text = output_of(result)
    expect_ordered(
        text,
        ["startup:", "running:", "loop finished at tick 120", "shutdown"],
        "a bounded headless run",
    )


def case_headless_reaches_exact_tick(binary: str) -> None:
    # A tick count that is not a multiple of anything convenient, so an off-by-one in the
    # accumulator's commit shows up rather than cancelling out.
    result = run(binary, ["--headless", "--ticks", "37"])
    expect_exit(result, 0, "a 37-tick run")
    expect_contains(output_of(result), "loop finished at tick 37", "a 37-tick run")


def case_edit_round_trip(binary: str) -> None:
    # The edit path against the scene the application actually ships, with no device and no
    # window, so this runs on every platform in continuous integration. The unit tests cover
    # the commands; what this adds is the composition root — a history wired to the wrong
    # scene, or a demo scene that stopped having the shape the editor expects, passes every
    # unit test and fails here.
    result = run(binary, ["--edit-check"])
    expect_exit(result, 0, "--edit-check")
    expect_contains(
        output_of(result), "3 commands applied and undone, scene identical", "--edit-check"
    )


def case_unbounded_throughput(binary: str) -> None:
    # Unbounded ignores wall time, so this also proves the run is not silently waiting on a
    # clock it no longer has.
    result = run(binary, ["--headless", "--unbounded", "--ticks", "20000"])
    expect_exit(result, 0, "an unbounded run")
    expect_contains(output_of(result), "loop finished at tick 20000", "an unbounded run")


def case_headless_needs_a_bound(binary: str) -> None:
    # Without a window there is nothing that can ask it to quit, so an unbounded headless run
    # would never return. Refusing it is the behaviour; hanging is the bug.
    result = run(binary, ["--headless"])
    expect_exit(result, 1, "an unbounded headless run")
    expect_contains(output_of(result), "--frames", "an unbounded headless run")


def case_rejects_unknown_option(binary: str) -> None:
    result = run(binary, ["--headless", "--ticks", "10", "--not-an-option"])
    expect_exit(result, 1, "an unknown option")
    expect_contains(output_of(result), "not-an-option", "an unknown option")


def case_rejects_bad_values(binary: str) -> None:
    for args, what in (
        (["--headless", "--ticks", "10", "--tps", "0"], "a zero tick rate"),
        (["--headless", "--ticks", "10", "--grid", "0"], "a zero grid"),
        (["--headless", "--ticks", "10", "--log-level", "shouty"], "an unknown log level"),
    ):
        result = run(binary, args)
        expect_exit(result, 1, what)


def case_log_file(binary: str) -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory) / "run.log"
        result = run(binary, ["--headless", "--ticks", "30", "--log-file", str(path)])
        expect_exit(result, 0, "a run with a log file")

        if not path.exists():
            raise CheckFailed(f"a run with a log file: {path} was never written")
        text = path.read_text(encoding="utf-8", errors="replace")
        # The file has to carry the same ordered lifecycle the console does, because a
        # headless run in continuous integration is read from the file and nowhere else.
        expect_ordered(text, ["startup:", "running:", "shutdown"], "the log file")


def case_log_level_filters(binary: str) -> None:
    quiet = output_of(run(binary, ["--headless", "--ticks", "10", "--log-level", "error"]))
    noisy = output_of(run(binary, ["--headless", "--ticks", "10", "--log-level", "info"]))

    expect_contains(noisy, "startup:", "an info-level run")
    if "startup:" in quiet:
        raise CheckFailed(
            "an error-level run still logged startup, so the level is not filtering\n"
            f"--- output ---\n{quiet}"
        )


def case_window_under_dummy_driver(binary: str) -> None:
    # A real window, event pump and resize path, driven by a video backend that renders
    # nowhere. This is the only automatic coverage of the window path on a machine with no
    # display, and it is why --video-driver exists.
    result = run(binary, ["--video-driver", "dummy", "--frames", "30", "--no-render"])
    expect_exit(result, 0, "a run under the dummy video driver")
    expect_ordered(
        output_of(result), ["startup:", "running:", "shutdown"],
        "a run under the dummy video driver",
    )



def case_gamepad_follows_the_window(binary: str) -> None:
    # The check M11 needed and did not have. Gamepad support was built, tested and reported
    # as met while neither application ever asked for the subsystem, because every gamepad
    # test builds its own platform and so none of them can see what a composition root
    # requested. This reads the one line that says what was actually brought up.
    windowed = run(binary, ["--video-driver", "dummy", "--frames", "5", "--no-render"])
    expect_exit(windowed, 0, "a windowed run")
    expect_contains(output_of(windowed), "gamepad on", "a windowed run enables the gamepad")

    # Off without a window: nothing there can aim a camera, and enumerating input devices is
    # work with no consumer that can also raise a permission prompt.
    bare = run(binary, ["--headless", "--ticks", "5"])
    expect_exit(bare, 0, "a headless run")
    expect_contains(output_of(bare), "gamepad off", "a headless run leaves the gamepad alone")

    opted_out = run(binary, ["--video-driver", "dummy", "--frames", "5", "--no-render",
                             "--no-gamepad"])
    expect_exit(opted_out, 0, "a windowed run with --no-gamepad")
    expect_contains(output_of(opted_out), "gamepad off", "--no-gamepad turns it off")


CASES = {
    "version": case_version,
    "help": case_help,
    "headless_lifecycle": case_headless_lifecycle,
    "headless_reaches_exact_tick": case_headless_reaches_exact_tick,
    "edit_round_trip": case_edit_round_trip,
    "unbounded_throughput": case_unbounded_throughput,
    "headless_needs_a_bound": case_headless_needs_a_bound,
    "rejects_unknown_option": case_rejects_unknown_option,
    "rejects_bad_values": case_rejects_bad_values,
    "log_file": case_log_file,
    "log_level_filters": case_log_level_filters,
    "window_under_dummy_driver": case_window_under_dummy_driver,
    "gamepad_follows_the_window": case_gamepad_follows_the_window,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="path to atlas_sandbox")
    parser.add_argument("--case", required=True, choices=sorted(CASES), help="which check")
    args = parser.parse_args()

    if not pathlib.Path(args.binary).exists():
        print(f"sandbox binary not found: {args.binary}", file=sys.stderr)
        return 1

    try:
        CASES[args.case](args.binary)
    except CheckFailed as failure:
        print(f"FAILED {args.case}: {failure}", file=sys.stderr)
        return 1

    print(f"ok {args.case}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
