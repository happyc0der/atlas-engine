#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end checks that run the Strategy Lab binary.

The lab is a composition root like the sandbox, so the same argument holds: construction
order, shutdown order, exit codes and lifecycle logging exist only when the whole program
runs. It also carries the milestone's determinism claims, which are worth nothing unless a
second run, a save and a replay actually reproduce them. Several checks here are designed to
fail without a specific piece of the design:

  seed_changes_hash       without it, deterministic_hash would pass on a constant
  load_continues          catches the two tick counters drifting apart after a load
  replay_detects_tampering  without it, a playback that ignored its own log would pass
  pick_returns_expected_cell  reimplements the chunk-major index formula independently

Run one with:
    python3 tests/integration/lab_checks.py --binary <path> --case <name>
"""
from __future__ import annotations

import argparse
import math
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import time

TIMEOUT_SECONDS = 240
SMALL = ["--grid", "32", "--chunk", "8"]


class CheckFailed(Exception):
    pass


def run(binary: str, args: list[str], cwd: str | None = None) -> subprocess.CompletedProcess:
    try:
        return subprocess.run([binary, *args], capture_output=True, text=True,
                              timeout=TIMEOUT_SECONDS, cwd=cwd, check=False)
    except subprocess.TimeoutExpired as expired:
        raise CheckFailed(f"{' '.join(args)} did not finish within {TIMEOUT_SECONDS}s") from expired


class Session:
    """Two lab processes talking over a socket, and the cleanup that makes them safe to run.

    **Nothing else in this harness manages two processes at once**, and the reasons it is worth
    writing carefully rather than inline are all failure modes rather than features:

    - The listener chooses its own port and prints it. Asking for a constant would make two
      cases running concurrently fight over it, and the only thing worse than a flaky network
      test is a flaky network test that is flaky because of another test.
    - Either process can fail before the other starts, so the port is waited for with a
      deadline rather than a sleep, and a listener that dies is noticed instead of waited on.
    - **Both are killed on any exit path.** A test that raises while a peer is still running
      leaves an orphan holding a port, and the next run of the same case then fails for a
      reason that has nothing to do with the code under test.
    """

    #: Long enough for a loaded continuous-integration runner, short enough that a hung pair
    #: fails inside the harness rather than at its outer timeout with nothing to read.
    START_SECONDS = 60.0
    RUN_SECONDS = 180.0

    PORT_LINE = re.compile(r"^listening on port (\d+)$", re.MULTILINE)

    def __init__(self, binary: str, shared: list[str]):
        self.binary = binary
        self.shared = shared
        self.listener: subprocess.Popen | None = None
        self.connector: subprocess.Popen | None = None
        self.listener_log = tempfile.NamedTemporaryFile(mode="w+", suffix=".listener",
                                                        delete=False)
        self.connector_log = tempfile.NamedTemporaryFile(mode="w+", suffix=".connector",
                                                         delete=False)
        self._listener_final = ""
        self._connector_final = ""

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, *_: object) -> None:
        for process in (self.listener, self.connector):
            if process is not None and process.poll() is None:
                process.kill()
                process.wait(timeout=10)
        # Cached before the files go, because a caller naturally reads the output *after* the
        # block that guaranteed the processes were cleaned up -- and every assertion worth
        # writing is about what they said.
        self._listener_final = self._text(self.listener_log)
        self._connector_final = self._text(self.connector_log)
        for handle in (self.listener_log, self.connector_log):
            handle.close()
            pathlib.Path(handle.name).unlink(missing_ok=True)

    def start(self, listener_extra: list[str] | None = None,
              connector_extra: list[str] | None = None) -> None:
        """Start the listener, learn its port, then start the connector."""
        self.listener = subprocess.Popen(
            [self.binary, "--headless", "--listen", *self.shared, *(listener_extra or [])],
            stdout=self.listener_log, stderr=subprocess.STDOUT, text=True)

        port = self._await_port()
        self.connector = subprocess.Popen(
            [self.binary, "--headless", "--connect", f"127.0.0.1:{port}", *self.shared,
             *(connector_extra or [])],
            stdout=self.connector_log, stderr=subprocess.STDOUT, text=True)

    def _await_port(self) -> int:
        deadline = time.monotonic() + self.START_SECONDS
        while time.monotonic() < deadline:
            # Checked before the log, so a listener that failed to bind is reported as having
            # exited rather than waited on until the deadline.
            if self.listener is not None and self.listener.poll() is not None:
                raise CheckFailed(f"the listener exited before binding:\n{self.listener_text()}")
            match = self.PORT_LINE.search(self.listener_text())
            if match:
                return int(match.group(1))
            time.sleep(0.1)
        raise CheckFailed(f"the listener never printed a port:\n{self.listener_text()}")

    def wait(self) -> tuple[int, int]:
        """Collect both, and return their exit codes as (listener, connector)."""
        deadline = time.monotonic() + self.RUN_SECONDS
        codes: list[int | None] = [None, None]
        while time.monotonic() < deadline and None in codes:
            for index, process in enumerate((self.listener, self.connector)):
                if codes[index] is None and process is not None:
                    codes[index] = process.poll()
            time.sleep(0.05)
        if None in codes:
            raise CheckFailed(
                f"a peer did not finish within {self.RUN_SECONDS}s\n"
                f"listener:\n{self.listener_text()}\nconnector:\n{self.connector_text()}")
        return (codes[0], codes[1])

    def _text(self, handle) -> str:
        if handle.closed:
            return ""
        handle.flush()
        return pathlib.Path(handle.name).read_text(encoding="utf-8", errors="replace")

    def listener_text(self) -> str:
        return self._listener_final or self._text(self.listener_log)

    def connector_text(self) -> str:
        return self._connector_final or self._text(self.connector_log)


def output_of(result: subprocess.CompletedProcess) -> str:
    return result.stdout + result.stderr


def expect_exit(result: subprocess.CompletedProcess, code: int, what: str) -> None:
    if result.returncode != code:
        raise CheckFailed(f"{what}: expected exit {code}, got {result.returncode}\n"
                          f"--- output ---\n{output_of(result)}")


def expect_contains(text: str, needle: str, what: str) -> None:
    if needle not in text:
        raise CheckFailed(f"{what}: expected to find {needle!r}\n--- output ---\n{text}")


def expect_ordered(text: str, needles: list[str], what: str) -> None:
    position = 0
    for needle in needles:
        found = text.find(needle, position)
        if found < 0:
            raise CheckFailed(f"{what}: expected {needle!r} after position {position}\n"
                              f"--- output ---\n{text}")
        position = found + len(needle)


def final_hash(text: str, what: str) -> str:
    match = re.search(r"final tick=(\d+) state hash=(0x[0-9a-f]+)", text)
    if not match:
        raise CheckFailed(f"{what}: no final hash line\n--- output ---\n{text}")
    return match.group(2)


def final_tick(text: str, what: str) -> int:
    match = re.search(r"final tick=(\d+)", text)
    if not match:
        raise CheckFailed(f"{what}: no final tick line\n--- output ---\n{text}")
    return int(match.group(1))


def headless(binary: str, *extra: str, ticks: int = 60, seed: int = 1) -> subprocess.CompletedProcess:
    return run(binary, ["--headless", *SMALL, "--unbounded", "--ticks", str(ticks),
                        "--seed", str(seed), *extra])


# ------------------------------------------------------------------ lifecycle

def check_version(binary: str) -> None:
    result = run(binary, ["--version"])
    expect_exit(result, 0, "--version")
    expect_contains(result.stdout, "Atlas", "--version")


def check_help(binary: str) -> None:
    result = run(binary, ["--help"])
    expect_exit(result, 0, "--help")
    for flag in ("--grid", "--ticks", "--play", "--record", "--pick", "--map-mode"):
        expect_contains(result.stdout, flag, "--help")


def check_headless_lifecycle(binary: str) -> None:
    result = headless(binary, ticks=30)
    expect_exit(result, 0, "headless run")
    expect_ordered(output_of(result),
                   ["startup:", "world:", "running:", "final tick=30", "observed", "shutdown"],
                   "lifecycle order")


def check_headless_reaches_exact_tick(binary: str) -> None:
    # Unbounded runs whole batches; without the clamp this overshoots.
    result = headless(binary, ticks=20000)
    expect_exit(result, 0, "exact tick")
    if final_tick(output_of(result), "exact tick") != 20000:
        raise CheckFailed(f"expected exactly 20000 ticks\n{output_of(result)}")


def check_headless_needs_a_bound(binary: str) -> None:
    result = run(binary, ["--headless", *SMALL])
    expect_exit(result, 1, "unbounded headless")
    expect_contains(output_of(result), "bound", "unbounded headless")


def check_catch_up_is_bounded(binary: str) -> None:
    # At an impossible tick rate, one tick per frame at most: the frame count must be at
    # least the tick count, which proves the limit held rather than eight ticks per frame.
    result = run(binary, ["--headless", *SMALL, "--tps", "100000", "--ticks", "400",
                          "--max-ticks-per-frame", "1"])
    expect_exit(result, 0, "catch-up")
    match = re.search(r"frames=(\d+) ticks=(\d+) dropped=", output_of(result))
    if not match or int(match.group(1)) < int(match.group(2)):
        raise CheckFailed(f"expected frames >= ticks with a limit of one\n{output_of(result)}")


# ------------------------------------------------------------------ determinism

def check_deterministic_hash(binary: str) -> None:
    a = headless(binary, "--commands-per-tick", "2", seed=5)
    b = headless(binary, "--commands-per-tick", "2", seed=5)
    expect_exit(a, 0, "first run")
    expect_exit(b, 0, "second run")
    if final_hash(output_of(a), "a") != final_hash(output_of(b), "b"):
        raise CheckFailed("the same seed produced different hashes")


def check_seed_changes_hash(binary: str) -> None:
    a = headless(binary, seed=5)
    b = headless(binary, seed=6)
    if final_hash(output_of(a), "a") == final_hash(output_of(b), "b"):
        raise CheckFailed("different seeds produced the same hash; deterministic_hash is vacuous")


def check_commands_change_hash(binary: str) -> None:
    a = headless(binary, seed=5)
    b = headless(binary, "--commands-per-tick", "2", seed=5)
    if final_hash(output_of(a), "a") == final_hash(output_of(b), "b"):
        raise CheckFailed("commands did not change the state hash; they are not reaching state")


# ------------------------------------------------------------------ save and load

def check_save_writes_file(binary: str) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        save = pathlib.Path(tmp) / "lab.sav"
        result = headless(binary, "--save", str(save), ticks=20)
        expect_exit(result, 0, "save")
        if not save.exists() or save.stat().st_size < 64:
            raise CheckFailed("no save file was written")
        if list(pathlib.Path(tmp).glob("*.tmp")):
            raise CheckFailed("a temporary was left behind after an atomic write")


def check_load_continues(binary: str) -> None:
    # 200 ticks, save, load, 100 more, against one 300-tick run, with commands on the way.
    with tempfile.TemporaryDirectory() as tmp:
        save = pathlib.Path(tmp) / "lab.sav"
        straight = headless(binary, "--commands-per-tick", "2", ticks=300, seed=9)
        first = headless(binary, "--commands-per-tick", "2", "--save", str(save), ticks=200, seed=9)
        second = headless(binary, "--commands-per-tick", "2", "--load", str(save), ticks=300, seed=9)
        for r, what in ((straight, "straight"), (first, "first"), (second, "second")):
            expect_exit(r, 0, what)
        expect_contains(output_of(second), "loaded", "second run loaded")
        if final_tick(output_of(second), "second") != 300:
            raise CheckFailed("the loaded run did not continue to tick 300")
        if final_hash(output_of(straight), "straight") != final_hash(output_of(second), "second"):
            raise CheckFailed("save, load and continue diverged from an uninterrupted run")


def check_load_different_grid_adopts_layout(binary: str) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        save = pathlib.Path(tmp) / "lab.sav"
        expect_exit(headless(binary, "--save", str(save), ticks=10), 0, "save 32x32")
        result = run(binary, ["--headless", "--grid", "64", "--chunk", "16", "--unbounded",
                              "--ticks", "20", "--load", str(save)])
        expect_exit(result, 0, "load into a different grid")
        expect_contains(output_of(result), "rebuilding geometry", "layout adopted")
        expect_contains(output_of(result), "1024 cells", "the loaded 32x32 is what continued")


def check_refuses_truncated_save(binary: str) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        save = pathlib.Path(tmp) / "lab.sav"
        expect_exit(headless(binary, "--save", str(save), ticks=10), 0, "save")
        data = save.read_bytes()
        save.write_bytes(data[: len(data) // 2])
        result = headless(binary, "--load", str(save), ticks=20)
        expect_exit(result, 1, "truncated save")
        expect_contains(output_of(result), "loading", "truncated save")


def check_refuses_mismatched_grid(binary: str) -> None:
    # The grid row claims 64 wide while the cells table has 32x32 rows. Still a valid layout
    # on its own, so only the cross-table check, or the integrity hash, can refuse it.
    with tempfile.TemporaryDirectory() as tmp:
        save = pathlib.Path(tmp) / "lab.sav"
        expect_exit(headless(binary, "--save", str(save), ticks=10, seed=1), 0, "save")
        data = bytearray(save.read_bytes())
        row = struct.pack("<IIIQ", 32, 32, 8, 1)  # width, height, chunk, seed
        at = data.find(row)
        if at < 0:
            raise CheckFailed("could not locate the grid row in the save")
        data[at:at + 4] = struct.pack("<I", 64)
        save.write_bytes(bytes(data))
        result = headless(binary, "--load", str(save), ticks=20)
        expect_exit(result, 1, "mismatched grid")
        expect_contains(output_of(result), "load", "mismatched grid")


# ------------------------------------------------------------------ replay

def parse_replay(data: bytes) -> tuple[list[tuple[int, int]], int]:
    """Return [(payload_offset, target_tick)] per command and the offset of the checkpoints."""
    magic, fmt, alg, seed, first_tick, tick_count, initial = struct.unpack_from("<QIIQQQQ", data, 0)
    at = 48
    (count,) = struct.unpack_from("<Q", data, at)
    at += 8
    commands = []
    for _ in range(count):
        target, source, sequence, ctype, length = struct.unpack_from("<QIQIQ", data, at)
        at += 32
        commands.append((at, target))
        at += length
    return commands, at


def check_record_then_play_matches(binary: str) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        replay = pathlib.Path(tmp) / "lab.replay"
        rec = headless(binary, "--commands-per-tick", "3", "--record", str(replay), ticks=120, seed=4)
        expect_exit(rec, 0, "record")
        expect_contains(output_of(rec), "replay written", "record")
        play = run(binary, ["--headless", *SMALL, "--seed", "4", "--play", str(replay)])
        expect_exit(play, 0, "play")
        expect_contains(output_of(play), "replay matched: 120 ticks", "play")
        if final_hash(output_of(rec), "rec") not in output_of(play):
            raise CheckFailed("the playback did not end at the recorded final hash")


def check_replay_detects_tampering(binary: str) -> None:
    # A semantic tamper: the last command's colour byte, kept in range so nothing but the
    # state hash can notice, and the divergence must be reported at that command's tick.
    with tempfile.TemporaryDirectory() as tmp:
        replay = pathlib.Path(tmp) / "lab.replay"
        expect_exit(headless(binary, "--commands-per-tick", "3", "--record", str(replay),
                             ticks=120, seed=4), 0, "record")
        data = bytearray(replay.read_bytes())
        commands, _ = parse_replay(bytes(data))
        if not commands:
            raise CheckFailed("the recording holds no commands to tamper with")
        payload_at, target = commands[-1]
        data[payload_at + 4] ^= 1
        replay.write_bytes(bytes(data))
        play = run(binary, ["--headless", *SMALL, "--seed", "4", "--play", str(replay)])
        expect_exit(play, 1, "tampered play")
        expect_contains(output_of(play), f"diverged at tick {target}", "tampered play")


def check_replay_refuses_wrong_seed(binary: str) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        replay = pathlib.Path(tmp) / "lab.replay"
        expect_exit(headless(binary, "--record", str(replay), ticks=30, seed=4), 0, "record")
        play = run(binary, ["--headless", *SMALL, "--seed", "5", "--play", str(replay)])
        expect_exit(play, 1, "wrong seed")
        expect_contains(output_of(play), "starts from state", "wrong seed")


# ------------------------------------------------------------------ arguments and logging

def check_rejects_unknown_option(binary: str) -> None:
    result = run(binary, ["--headless", "--ticks", "1", "--bogus"])
    expect_exit(result, 1, "unknown option")
    expect_contains(output_of(result), "bogus", "unknown option")


def check_rejects_bad_values(binary: str) -> None:
    result = run(binary, ["--headless", "--ticks", "1", "--grid", "100", "--chunk", "32"])
    expect_exit(result, 1, "grid not a multiple of chunk")
    expect_contains(output_of(result), "whole number of chunks", "grid not a multiple")
    result = run(binary, ["--headless", "--ticks", "1", *SMALL, "--map-mode", "bogus"])
    expect_exit(result, 1, "bad map mode")
    expect_contains(output_of(result), "map mode", "bad map mode")


def check_log_file(binary: str) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        log = pathlib.Path(tmp) / "lab.log"
        expect_exit(headless(binary, "--log-file", str(log), ticks=5), 0, "log file")
        expect_contains(log.read_text(), "final tick=5", "log file contents")


# ------------------------------------------------------------------ with a window

def check_window_under_dummy_driver(binary: str) -> None:
    result = run(binary, ["--video-driver", "dummy", "--no-render", *SMALL, "--frames", "10"])
    expect_exit(result, 0, "dummy driver")
    expect_ordered(output_of(result), ["window 'Atlas lab' created", "shutdown"], "dummy driver")


def chunk_major_index(x: int, y: int, width: int, chunk: int) -> int:
    # Written independently of GridLayout::cell_index on purpose.
    chunks_x = width // chunk
    chunk_id = (y // chunk) * chunks_x + (x // chunk)
    within = (y % chunk) * chunk + (x % chunk)
    return chunk_id * chunk * chunk + within


def check_pick_returns_expected_cell(binary: str) -> None:
    grid, chunk, cell_size = 64, 16, 8.0
    logical = (500.0, 300.0)
    # The dummy audio driver is asked for explicitly. A machine with no sound server does not
    # silently fall back to it: the subsystem fails to start, no device is opened, and the
    # audio assertion below would then be testing the runner rather than the engine. That is
    # exactly what happened the first time this case ran in the software-rasteriser container.
    result = run(binary, ["--grid", str(grid), "--chunk", str(chunk), "--frames", "12",
                          "--no-overlay", "--audio-driver", "dummy",
                          "--pick", f"{logical[0]:.0f},{logical[1]:.0f}"])
    expect_exit(result, 0, "pick run")
    text = output_of(result)
    window = re.search(r"pixels=(\d+)x(\d+) scale=(\d+(?:\.\d+)?)", text)
    if not window:
        raise CheckFailed(f"no window size in the log\n{text}")
    width_px, height_px, scale = int(window.group(1)), int(window.group(2)), float(window.group(3))
    # The field's fit: the grid centred, zoomed to 95% of what fits.
    grid_world = grid * cell_size
    zoom = min(width_px / grid_world, height_px / grid_world) * 0.95
    centre = (grid_world / 2, grid_world / 2)
    px = (logical[0] * scale, logical[1] * scale)
    world = (centre[0] + (px[0] - width_px / 2) / zoom, centre[1] + (px[1] - height_px / 2) / zoom)
    cx, cy = math.floor(world[0] / cell_size), math.floor(world[1] / cell_size)
    if not (0 <= cx < grid and 0 <= cy < grid):
        raise CheckFailed(f"the chosen point ({cx}, {cy}) is outside the grid; pick another")
    expected = chunk_major_index(cx, cy, grid, chunk)
    expect_contains(text, f"identifier pass={expected} analytic={expected}", "pick agrees")
    expect_contains(text, "pick disagreements=0", "no disagreements")
    expect_contains(text, f"picked cell {expected}", "the pick became a command")
    # And the pick made a sound. This is the whole audio path through its real call site: a
    # device opened, a generated clip created, a voice started where a person clicked, and the
    # mixer advanced it without the queue running dry.
    expect_contains(text, "audio: 1 voices peak, 0 underruns", "the pick made a sound")



def check_gamepad_follows_the_window(binary: str) -> None:
    # The check M11 needed and did not have. Gamepad support was built, tested and reported
    # as met while neither application ever asked for the subsystem, because every gamepad
    # test builds its own platform and so none of them can see what a composition root
    # requested. This reads the one line that says what was actually brought up.
    windowed = run(binary, [*SMALL, "--video-driver", "dummy", "--frames", "5",
                            "--no-render"])
    expect_exit(windowed, 0, "a windowed run")
    expect_contains(output_of(windowed), "gamepad on", "a windowed run enables the gamepad")

    # Off without a window: nothing there can aim a camera, and enumerating input devices is
    # work with no consumer that can also raise a permission prompt.
    bare = run(binary, [*SMALL, "--headless", "--ticks", "5"])
    expect_exit(bare, 0, "a headless run")
    expect_contains(output_of(bare), "gamepad off", "a headless run leaves the gamepad alone")

    opted_out = run(binary, [*SMALL, "--video-driver", "dummy", "--frames", "5",
                             "--no-render", "--no-gamepad"])
    expect_exit(opted_out, 0, "a windowed run with --no-gamepad")
    expect_contains(output_of(opted_out), "gamepad off", "--no-gamepad turns it off")



def check_audio_follows_the_window(binary: str) -> None:
    # Audio follows the window for the same reason the gamepad does: a run with no window has
    # nobody to hear it, and opening an output device on a build machine is work with no
    # consumer. What this reads is the one line that says what was brought up, plus the
    # device's own line, plus the summary that proves the device was still there at the end.
    windowed = run(binary, [*SMALL, "--video-driver", "dummy", "--audio-driver", "dummy",
                            "--frames", "5", "--no-render"])
    expect_exit(windowed, 0, "a windowed run")
    expect_ordered(output_of(windowed),
                   ["audio on", "audio ready:", "audio: 0 voices peak"],
                   "a windowed run opens a device and reports on it at exit")

    bare = run(binary, [*SMALL, "--headless", "--ticks", "5"])
    expect_exit(bare, 0, "a headless run")
    expect_contains(output_of(bare), "audio off", "a headless run opens no device")
    if "audio ready:" in output_of(bare):
        raise CheckFailed("a headless run opened an audio device")

    opted_out = run(binary, [*SMALL, "--video-driver", "dummy", "--frames", "5", "--no-render",
                             "--no-audio"])
    expect_exit(opted_out, 0, "a windowed run with --no-audio")
    expect_contains(output_of(opted_out), "audio off", "--no-audio turns it off")
    if "audio ready:" in output_of(opted_out):
        raise CheckFailed("--no-audio opened an audio device anyway")


def loopback(binary: str, args: list[str]) -> subprocess.CompletedProcess:
    return run(binary, ["--headless", *SMALL, "--unbounded", *args])


def peer_hashes(text: str) -> list[str]:
    return re.findall(r"^peer \d+: final tick=\d+ state hash=(0x[0-9a-f]+)", text, re.M)


def check_loopback_peers_agree(binary: str) -> None:
    # Three peers over an in-memory link with latency and reordering, exchanging real messages
    # through the real codec, inbox, gate and session. The binary asserts the agreement itself
    # and exits non-zero if it fails; this checks the same thing from outside, and checks that
    # the agreement was not reached by nothing happening.
    result = loopback(binary, ["--ticks", "200", "--loopback-peers", "3", "--commands-per-tick", "2",
                       "--link-latency", "2", "--link-reorder", "7"])
    expect_exit(result, 0, "a three-peer loopback run")
    text = output_of(result)

    hashes = peer_hashes(text)
    if len(hashes) != 3:
        raise CheckFailed(f"expected three peer lines, got {len(hashes)}")
    if len(set(hashes)) != 1:
        raise CheckFailed(f"the peers finished at different hashes: {hashes}")

    expect_contains(text, "divergences=0", "no peer diverged")
    # Turns actually crossed the link. Without this the case passes on a session in which
    # nobody sent anything and everybody trivially agreed.
    if not re.search(r"turns received=[1-9]", text):
        raise CheckFailed("no peer received a turn, so the agreement proves nothing")


def check_loopback_differs_from_solo(binary: str) -> None:
    # The anti-vacuity guard, and the same shape as seed_changes_hash: two peers apply twice as
    # many commands as one, so their final hash **must** differ from a solo run's. If it matched,
    # the second peer's commands are not reaching state — which is exactly what happens if a
    # peer stamps SourceId::Local instead of its own identifier and the two streams collide.
    common = ["--ticks", "200", "--commands-per-tick", "2"]
    solo = loopback(binary, common)
    expect_exit(solo, 0, "a solo run")
    pair = loopback(binary, common + ["--loopback-peers", "2"])
    expect_exit(pair, 0, "a two-peer run")

    solo_hash = final_hash(output_of(solo), "solo")
    pair_hash = peer_hashes(output_of(pair))
    if not pair_hash:
        raise CheckFailed("the loopback run printed no peer hashes")
    if solo_hash == pair_hash[0]:
        raise CheckFailed("a two-peer run reached the same state as a solo one, so the second "
                          "peer's commands are not reaching the simulation")


def check_loopback_held_turn_stalls_then_completes(binary: str) -> None:
    # A turn nobody can deliver must stop every peer at that tick rather than being skipped, and
    # releasing it must let both finish in the same state. The stall count is asserted non-zero
    # because a hold that never held would pass every other assertion here.
    result = loopback(binary, ["--ticks", "300", "--loopback-peers", "2", "--commands-per-tick", "2",
                       "--loopback-fault", "hold:100", "--loopback-release-after", "50"])
    expect_exit(result, 0, "a held turn that is later released")
    text = output_of(result)
    if "diverged" in text:
        raise CheckFailed("a held turn produced a divergence; it should only have stalled")
    expect_contains(text, "releasing held messages", "the hold was actually held and released")

    hashes = peer_hashes(text)
    if len(set(hashes)) != 1:
        raise CheckFailed(f"the peers finished at different hashes after a stall: {hashes}")
    if re.search(r"stalls=0\b", text):
        raise CheckFailed("nothing stalled, so the hold did nothing")


def check_loopback_unreleased_hold_fails_fast(binary: str) -> None:
    # Lockstep does not degrade gracefully: it waits. A turn that is never delivered stops the
    # session for ever, so the binary bounds its own wait and says what is waiting for what —
    # otherwise this would be a timeout with no message rather than a failure.
    result = loopback(binary, ["--ticks", "400", "--loopback-peers", "2", "--commands-per-tick", "2",
                       "--loopback-fault", "hold:100"])
    expect_exit(result, 1, "an unreleased hold")
    expect_contains(output_of(result), "waiting on source", "the stall says what it waits for")


def check_loopback_corrupt_turn_is_caught(binary: str) -> None:
    # One flipped bit inside a command payload. It is applied by the sender and differently by
    # the receiver, which is precisely the divergence the hash checks exist to catch. What must
    # never happen is exit zero.
    result = loopback(binary, ["--ticks", "300", "--loopback-peers", "2", "--commands-per-tick", "2",
                       "--loopback-fault", "corrupt:100"])
    expect_exit(result, 1, "a corrupted turn")
    text = output_of(result)
    expect_contains(text, "diverged at tick", "the corruption was detected")
    # Attributed to a system, not merely detected: "something diverged at tick 112" is not a
    # starting point for anybody.
    expect_contains(text, "the first system whose writes differ", "the divergence names a system")


def check_mod_runs_and_changes_state(binary: str) -> None:
    """The sandbox end to end: a mod loads, submits, and the simulation notices."""
    with_mod = headless(binary, "--mod", "synthetic.wasm", ticks=40)
    expect_exit(with_mod, 0, "a run with a mod")
    text = output_of(with_mod)
    expect_contains(text, "mod 'synthetic.wasm' loaded", "the mod loaded")
    # One command a tick, none refused, and still running at the end. A mod that was disabled
    # at tick three and one that simply had nothing to say both submit few commands, so the
    # state at the end is what separates them.
    expect_contains(text, "40 submitted, 0 refused", "the mod's own accounting")
    expect_contains(text, "still running", "the mod survived the run")

    # The anti-vacuity check, and the reason this case is not two cases. A mod that loads,
    # runs, reports and changes nothing would pass every assertion above.
    without = headless(binary, ticks=40)
    expect_exit(without, 0, "a run without a mod")
    mod_hash = final_hash(text, "a run with a mod")
    plain_hash = final_hash(output_of(without), "a run without one")
    if mod_hash == plain_hash:
        raise CheckFailed(f"the mod changed nothing: both runs ended at {mod_hash}")

    # And it is deterministic, which is the property lockstep will rest on in the next slice.
    again = headless(binary, "--mod", "synthetic.wasm", ticks=40)
    expect_exit(again, 0, "a repeated run with a mod")
    if final_hash(output_of(again), "a repeated run") != mod_hash:
        raise CheckFailed("two runs of the same mod disagreed")


def check_mod_path_is_validated(binary: str) -> None:
    """A mod's name is untrusted input, and is refused rather than resolved."""
    escape = headless(binary, "--mod", "../../../etc/passwd", ticks=1)
    expect_exit(escape, 1, "a mod name that climbs out of the mount")
    # Refused by VirtualPath before anything opens a file, which is why the message is about the
    # path rather than about a file that could not be read.
    expect_contains(output_of(escape), "mod", "the refusal names the mod")

    missing = headless(binary, "--mod", "no-such-mod.wasm", ticks=1)
    expect_exit(missing, 1, "a mod that is not there")


def check_mod_is_not_a_peer(binary: str) -> None:
    """A mod's commands are its own, and its identifier is not in the peer range."""
    result = headless(binary, "--mod", "synthetic.wasm", ticks=5)
    expect_exit(result, 0, "a run with a mod")
    # 2147483648 is bit 31 alone: the first mod identifier, and one no peer index can reach.
    expect_contains(output_of(result), "loaded as source 2147483648", "the mod's identifier")


def check_mod_replay_needs_no_runtime(binary: str) -> None:
    """Proof (a): a recording carries a mod's commands, and replaying needs no sandbox.

    This is ADR-0009 decision 2 paying off. A mod's output is commands, commands are recorded,
    and a replay feeds them back — so a recording made with a mod plays back identically on a
    build that never loads one. Nothing here passes --mod to the replay.
    """
    with tempfile.TemporaryDirectory() as tmp:
        replay = pathlib.Path(tmp) / "mod.replay"
        rec = headless(binary, "--mod", "synthetic.wasm", "--record", str(replay), ticks=40, seed=4)
        expect_exit(rec, 0, "recording a run with a mod")
        recorded = final_hash(output_of(rec), "the recorded run")

        play = run(binary, ["--headless", *SMALL, "--seed", "4", "--play", str(replay)])
        expect_exit(play, 0, "replaying without the mod")
        text = output_of(play)
        expect_contains(text, "replay matched", "the replay matched")
        expect_contains(text, recorded, "the replay ended where the recording did")
        # And it really did run without one: no runtime, no load, no sandbox.
        if "script runtime ready" in text:
            raise CheckFailed(f"the replay started a script runtime\n--- output ---\n{text}")


def check_mod_peers_agree(binary: str) -> None:
    """Proof (b): two peers running the same mod agree, without exchanging its commands."""
    result = run(binary, ["--headless", *SMALL, "--unbounded", "--ticks", "60", "--seed", "4",
                          "--loopback-peers", "2", "--mod", "synthetic.wasm",
                          "--link-latency", "2", "--link-reorder", "7"])
    expect_exit(result, 0, "two peers with a mod")
    text = output_of(result)
    expect_contains(text, "divergences=0", "no divergence")
    # Both mods ran every tick and neither was disabled, which is what makes the agreement mean
    # something: two mods that both stopped early would also agree.
    expect_contains(text, "peer 0 mod 'synthetic.wasm': 60 submitted, 0 refused", "peer 0's mod")
    expect_contains(text, "peer 1 mod 'synthetic.wasm': 60 submitted, 0 refused", "peer 1's mod")

    # Exactly 58, and the number is the assertion rather than decoration. A mod decides **before**
    # the tick it is called for, so over 60 ticks at delay 2 it stamps ticks 2 to 61 and 58 of
    # those actually run. A mod called after the step instead would stamp 3 to 62 and land 57 —
    # still deterministic, still agreed between peers, and a different simulation. Agreement
    # cannot catch that; a count can.
    expect_contains(text, "commands applied=58", "the mod decided before the tick, not after")

    # The anti-vacuity half: the same run with no mod ends somewhere else, so the agreement
    # above is an agreement about the mod's commands rather than about an empty simulation.
    without = run(binary, ["--headless", *SMALL, "--unbounded", "--ticks", "60", "--seed", "4",
                           "--loopback-peers", "2", "--link-latency", "2", "--link-reorder", "7"])
    expect_exit(without, 0, "two peers without a mod")
    if _peer_hash(text) == _peer_hash(output_of(without)):
        raise CheckFailed("the mod changed nothing in a loopback run")


def _peer_hash(text: str) -> str:
    match = re.search(r"peer 0: final tick=\d+ state hash=(0x[0-9a-f]+)", text)
    if not match:
        raise CheckFailed(f"no peer 0 hash line\n--- output ---\n{text}")
    return match.group(1)


def check_mod_clock_diverges(binary: str) -> None:
    """Proof (c): why the interface has no clock.

    A mod that reads a host clock decides differently on each machine. Without the unsafe flag
    it cannot even load; with it, two peers disagree and the session stops. **This case fails if
    anybody ever adds a clock to the guest interface for real**, which is the point of keeping
    it.
    """
    refused = headless(binary, "--mod", "clock.wasm", ticks=5)
    expect_exit(refused, 1, "a clock mod with no flag")
    expect_contains(output_of(refused), "atlas_debug_clock_ns", "the refusal names the import")
    expect_contains(output_of(refused), "does not provide", "the host does not provide it")

    diverged = run(binary, ["--headless", *SMALL, "--unbounded", "--ticks", "200", "--seed", "4",
                            "--loopback-peers", "2", "--mod", "clock.wasm",
                            "--unsafe-debug-imports"])
    expect_exit(diverged, 1, "a clock mod under lockstep")
    text = output_of(diverged)
    expect_contains(text, "diverged at tick", "the divergence was detected")
    # Attributed, not merely noticed: the whole reason hash checks carry per-system hashes.
    expect_contains(text, "the first system whose writes differ", "the divergence names a system")
    expect_contains(text, "UNSAFE debug imports", "the run said what it was doing")


def check_socket_peers_agree(binary: str) -> None:
    """Two processes, one socket, the same final hash."""
    # **This is the only case in the repository where two operating-system processes talk to
    # each other**, and it is what the in-memory link cannot prove: a loopback never loses a
    # packet, never refuses a connection, and never has a process die on the other end.
    shared = [*SMALL, "--ticks", "120", "--commands-per-tick", "2"]
    with Session(binary, shared) as session:
        session.start()
        listener_code, connector_code = session.wait()

    listener = session.listener_text()
    connector = session.connector_text()
    if listener_code != 0 or connector_code != 0:
        raise CheckFailed(
            f"a peer exited non-zero (listener {listener_code}, connector {connector_code})\n"
            f"listener:\n{listener}\nconnector:\n{connector}")

    listener_hashes = peer_hashes(listener)
    connector_hashes = peer_hashes(connector)
    if len(listener_hashes) != 1 or len(connector_hashes) != 1:
        raise CheckFailed("each process should print exactly one peer line\n"
                          f"listener:\n{listener}\nconnector:\n{connector}")
    if listener_hashes[0] != connector_hashes[0]:
        raise CheckFailed(f"the two processes finished at different hashes: "
                          f"{listener_hashes[0]} and {connector_hashes[0]}")

    # They are different peers, not the same one twice. Without this the case would pass on a
    # bug where both processes somehow took index 0 and agreed with themselves.
    if "source=0" not in listener or "source=1" not in connector:
        raise CheckFailed("the listener should be source 0 and the connector source 1\n"
                          f"listener:\n{listener}\nconnector:\n{connector}")

    # And turns actually crossed the socket. An agreement reached by nobody sending anything is
    # not an agreement about anything.
    for name, text in (("listener", listener), ("connector", connector)):
        if not re.search(r"turns received=[1-9]", text):
            raise CheckFailed(f"the {name} received no turns, so the agreement proves nothing")
        if not re.search(r"agreed hashes=[1-9]", text):
            raise CheckFailed(f"the {name} agreed no hash checks")


def check_socket_differs_from_solo(binary: str) -> None:
    """The anti-vacuity half: two peers must not reach a solo run's hash."""
    # Two peers submit two command streams and a solo run submits one, so the states must
    # differ. If they did not, the case above would be satisfied by a session that ignored
    # everything it received -- which is exactly the bug it exists to catch.
    shared = [*SMALL, "--ticks", "120", "--commands-per-tick", "2"]
    with Session(binary, shared) as session:
        session.start()
        listener_code, connector_code = session.wait()
    if listener_code != 0 or connector_code != 0:
        # With the logs, because without them a failure here says only that something went
        # wrong. This case failed once on a continuous-integration runner and could not be
        # diagnosed from its own output, which is a defect in the check rather than in the code
        # it was checking -- its sibling above had said this properly all along.
        raise CheckFailed(
            f"the socket pair did not finish cleanly "
            f"(listener {listener_code}, connector {connector_code})\n"
            f"listener:\n{session.listener_text()}\nconnector:\n{session.connector_text()}")
    paired = peer_hashes(session.listener_text())

    solo = run(binary, ["--headless", *shared])
    expect_exit(solo, 0, "a solo run")
    solo_hashes = peer_hashes(output_of(solo)) or re.findall(r"state hash=(0x[0-9a-f]+)",
                                                             output_of(solo))
    if solo_hashes and paired and solo_hashes[0] == paired[0]:
        raise CheckFailed("a two-peer session reached the same hash as a solo run, so the "
                          "second peer's commands changed nothing")


def check_socket_killed_peer_ends_the_session(binary: str) -> None:
    """A peer that dies ends the session rather than hanging the other one."""
    # ADR-0017 D5. The connector is killed outright, so it never says goodbye -- which is the
    # case the destructor's graceful close cannot cover and the transport's deadline must.
    shared = [*SMALL, "--ticks", "100000", "--commands-per-tick", "1"]
    with Session(binary, shared) as session:
        session.start()

        # Let the handshake finish, so this kills a running session rather than a starting one.
        deadline = time.monotonic() + 60.0
        while time.monotonic() < deadline:
            if "socket handshake agreed" in session.listener_text():
                break
            time.sleep(0.1)
        else:
            raise CheckFailed(f"the handshake never completed:\n{session.listener_text()}")

        session.connector.kill()
        session.connector.wait(timeout=10)

        # The listener must notice and stop. A transport that waited for ever would hang here
        # until the harness killed it, which is the failure this asserts against.
        deadline = time.monotonic() + 90.0
        while time.monotonic() < deadline and session.listener.poll() is None:
            time.sleep(0.1)
        code = session.listener.poll()

    if code is None:
        raise CheckFailed("the listener never noticed its peer had gone\n"
                          f"{session.listener_text()}")
    if code == 0:
        raise CheckFailed("the listener exited zero after losing its peer; a session that lost "
                          "a participant did not succeed")

    # **Either reason is correct, and asserting one of them was this case's own bug.** A killed
    # process never says goodbye, so whether the listener notices by its socket closing -- which
    # ENet reports as a disconnect -- or by nobody having been heard from depends on whether the
    # operating system delivered the close, and that varies by platform and by timing. It
    # noticed on macOS Debug by the first route and on macOS Release by the second.
    #
    # What the case is named for, and what matters, is that the session ended rather than
    # hanging. Pinning the mechanism asserted how it found out instead.
    text = session.listener_text()
    if "disconnected" not in text and "heard from" not in text:
        raise CheckFailed("the listener stopped without saying its peer had gone\n" + text)


CASES = {
    "version": check_version,
    "help": check_help,
    "headless_lifecycle": check_headless_lifecycle,
    "headless_reaches_exact_tick": check_headless_reaches_exact_tick,
    "headless_needs_a_bound": check_headless_needs_a_bound,
    "catch_up_is_bounded": check_catch_up_is_bounded,
    "deterministic_hash": check_deterministic_hash,
    "seed_changes_hash": check_seed_changes_hash,
    "commands_change_hash": check_commands_change_hash,
    "mod_runs_and_changes_state": check_mod_runs_and_changes_state,
    "mod_path_is_validated": check_mod_path_is_validated,
    "mod_is_not_a_peer": check_mod_is_not_a_peer,
    "mod_replay_needs_no_runtime": check_mod_replay_needs_no_runtime,
    "mod_peers_agree": check_mod_peers_agree,
    "mod_clock_diverges": check_mod_clock_diverges,
    "save_writes_file": check_save_writes_file,
    "load_continues": check_load_continues,
    "load_different_grid_adopts_layout": check_load_different_grid_adopts_layout,
    "refuses_truncated_save": check_refuses_truncated_save,
    "refuses_mismatched_grid": check_refuses_mismatched_grid,
    "loopback_peers_agree": check_loopback_peers_agree,
    "socket_peers_agree": check_socket_peers_agree,
    "socket_differs_from_solo": check_socket_differs_from_solo,
    "socket_killed_peer_ends_the_session": check_socket_killed_peer_ends_the_session,
    "loopback_differs_from_solo": check_loopback_differs_from_solo,
    "loopback_held_turn_stalls_then_completes": check_loopback_held_turn_stalls_then_completes,
    "loopback_unreleased_hold_fails_fast": check_loopback_unreleased_hold_fails_fast,
    "loopback_corrupt_turn_is_caught": check_loopback_corrupt_turn_is_caught,
    "record_then_play_matches": check_record_then_play_matches,
    "replay_detects_tampering": check_replay_detects_tampering,
    "replay_refuses_wrong_seed": check_replay_refuses_wrong_seed,
    "rejects_unknown_option": check_rejects_unknown_option,
    "rejects_bad_values": check_rejects_bad_values,
    "log_file": check_log_file,
    "window_under_dummy_driver": check_window_under_dummy_driver,
    "gamepad_follows_the_window": check_gamepad_follows_the_window,
    "audio_follows_the_window": check_audio_follows_the_window,
    "pick_returns_expected_cell": check_pick_returns_expected_cell,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", required=True, help="path to atlas_lab")
    parser.add_argument("--case", required=True, choices=sorted(CASES), help="which check")
    args = parser.parse_args()
    try:
        CASES[args.case](args.binary)
    except CheckFailed as failed:
        print(f"FAILED {args.case}: {failed}", file=sys.stderr)
        return 1
    print(f"ok {args.case}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
