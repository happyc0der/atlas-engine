# SPDX-License-Identifier: GPL-3.0-or-later
"""What every whole-program check file shares: running a binary, reading what it said, failing
with the output attached, and managing two processes that talk over a socket.

Lifted out of `lab_checks.py` in M22, when chess became the second application with its own
check file. The helpers had one caller until then, which is why they lived there; with two, a
copy would be the thing that drifts.
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import tempfile
import time

TIMEOUT_SECONDS = 240


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
