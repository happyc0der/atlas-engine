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
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

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



def case_audio_clip_loads_and_reloads(binary: str) -> None:
    # The whole asset path for a sound, end to end and through the real one: requested by
    # virtual path, read and decoded on a worker, finalised into a clip on the main thread,
    # resampled from the file's 22.05 kHz to the engine's 48 kHz, and played looping.
    #
    # Paced by --ticks rather than --frames. With the dummy driver and nothing to draw, three
    # hundred frames take under a millisecond, which is less time than a worker needs to read
    # a hundred and seventy kilobytes: the run would finish before the asset did, and the
    # check would be measuring the loop's speed rather than the pipeline.
    with tempfile.TemporaryDirectory() as directory:
        assets = pathlib.Path(directory) / "assets"
        shutil.copytree("assets/source", assets)
        loop = assets / "audio" / "ambient_loop.wav"
        if not loop.exists():
            raise CheckFailed("the generated ambient loop is missing; "
                              "run tools/gen_audio_assets.py")

        # Touched from a thread partway through, because hot reload polls once a second and
        # a file that changes before the run starts is just a file.
        def touch_later() -> None:
            time.sleep(2.0)
            loop.touch()

        toucher = threading.Thread(target=touch_later, daemon=True)
        toucher.start()
        result = run(binary, ["--video-driver", "dummy", "--no-render",
                              "--audio-driver", "dummy", "--assets-dir", str(assets),
                              "--hot-reload", "--ticks", "300"])
        toucher.join(timeout=5)

    expect_exit(result, 0, "a run with an audio asset")
    text = output_of(result)
    expect_ordered(text, ["audio ready:", "ambient loop playing", "asset(s) changed on disk",
                          "ambient loop playing"],
                   "the clip loads, plays, is reloaded, and plays again")
    expect_contains(text, "1 total, 1 ready, 0 failed", "the clip reached ready")

    # One clip after a reload, not two. A reload creates a new clip and must release the one it
    # displaced; without that the pool grows by one every time a file is touched, which is a leak
    # nothing else here would notice.
    #
    # The underrun count is deliberately not part of this. It used to be, because all three
    # numbers share a log line and matching the whole line was convenient — and then a loaded
    # machine produced one underrun and failed an assertion named "the displaced clip was
    # released", which is not what it checks. An underrun is a real-time property of the machine:
    # PERFORMANCE.md's M12 section says plainly that two consecutive long frames can click, by
    # design. Where that claim is worth making it is measured and recorded there, not asserted
    # here on whatever the continuous-integration runner happened to be doing.
    match = re.search(r"audio: (\d+) voices peak, (\d+) underruns, (\d+) clips", text)
    if match is None:
        raise CheckFailed(f"no audio summary line\n--- output ---\n{text}")
    if match.group(1) != "1" or match.group(3) != "1":
        raise CheckFailed(
            f"expected one voice and one clip after the reload, got {match.group(1)} voice(s) "
            f"and {match.group(3)} clip(s)\n--- output ---\n{text}")



def case_scene_composition(binary: str) -> None:
    # The demonstration scene had no automated coverage of any kind before M13: no test ran
    # --scene, its methods need a graphics device, and nothing in the repository compares
    # images. This pins the two things that must survive the milestone that changes how it
    # moves — the hierarchy it exists to demonstrate, and that an edit recomposes into the
    # drawn position — and says nothing about how anything moves.
    result = run(binary, ["--scene-check"])
    expect_exit(result, 0, "the scene check")
    expect_contains(output_of(result), "composition reaches depth 3",
                    "the scene still has three levels to compose through")
    expect_contains(output_of(result), "edits recompose", "an edit reaches the drawn position")


def case_animation_plays(binary: str) -> None:
    # M13's acceptance path, and the first check in the repository that runs the committed
    # animation clips. It goes through the real asset pipeline — the files are requested by
    # virtual path, read and parsed on a worker, finalised into the cache on the main thread,
    # and played — so it fails if a clip file stops parsing, if a clip resolves to an
    # identifier the scene does not record, or if a clip decodes and nothing claims it.
    #
    # The poses are compared against a table recorded in the check itself, to a tolerance
    # rather than to bytes: these pass through float easing, where whether a compiler contracts
    # a multiply and an add changes the last bits. The clock is integer and is compared
    # exactly. That split is deliberate and is written down at the assertion.
    result = run(binary, ["--anim-check"])
    expect_exit(result, 0, "the animation check")
    text = output_of(result)
    expect_contains(text, "poses match the recorded table",
                    "the clips play what the recorded table says they should")
    # The two-writer split of ADR-0012, at the level of the whole program: a scene can be
    # animated for four seconds and still save the bytes it started with, and an entity can be
    # dragged while a clip is moving it.
    expect_contains(text, "saved bytes unchanged", "animating the scene saves what it started with")
    expect_contains(text, "a drag while animating survives and undoes",
                    "an edit lands while a clip plays, and undo takes back only the edit")


CASES = {
    "version": case_version,
    "help": case_help,
    "headless_lifecycle": case_headless_lifecycle,
    "headless_reaches_exact_tick": case_headless_reaches_exact_tick,
    "edit_round_trip": case_edit_round_trip,
    "scene_composition": case_scene_composition,
    "animation_plays": case_animation_plays,
    "unbounded_throughput": case_unbounded_throughput,
    "headless_needs_a_bound": case_headless_needs_a_bound,
    "rejects_unknown_option": case_rejects_unknown_option,
    "rejects_bad_values": case_rejects_bad_values,
    "log_file": case_log_file,
    "log_level_filters": case_log_level_filters,
    "window_under_dummy_driver": case_window_under_dummy_driver,
    "gamepad_follows_the_window": case_gamepad_follows_the_window,
    "audio_clip_loads_and_reloads": case_audio_clip_loads_and_reloads,
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
