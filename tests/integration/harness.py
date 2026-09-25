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
    """Lab processes talking over a socket, and the cleanup that makes them safe to run.

    One listener and, by default, one connector. **Nothing else in this harness manages several
    processes at once**, and the reasons it is worth writing carefully rather than inline are
    all failure modes rather than features:

    - The listener chooses its own port and prints it. Asking for a constant would make two
      cases running concurrently fight over it, and the only thing worse than a flaky network
      test is a flaky network test that is flaky because of another test.
    - Any process can fail before the others start, so the port is waited for with a deadline
      rather than a sleep, and a listener that dies is noticed instead of waited on.
    - **Every process is killed on any exit path.** A test that raises while a peer is still
      running leaves an orphan holding a port, and the next run of the same case then fails for
      a reason that has nothing to do with the code under test.

    More than one connector since M25, when the listener began relaying (ADR-0022) and a
    session of three became possible. Before that no case had ever started three processes,
    which is how a lab that accepted `--expect 16` could fail at three unnoticed.
    """

    #: Long enough for a loaded continuous-integration runner, short enough that a hung pair
    #: fails inside the harness rather than at its outer timeout with nothing to read.
    START_SECONDS = 60.0
    RUN_SECONDS = 180.0

    PORT_LINE = re.compile(r"^listening on port (\d+)$", re.MULTILINE)

    def __init__(self, binary: str, shared: list[str], connectors: int = 1):
        self.binary = binary
        self.shared = shared
        self.listener: subprocess.Popen | None = None
        self.connectors: list[subprocess.Popen] = []
        self.listener_log = tempfile.NamedTemporaryFile(mode="w+", suffix=".listener",
                                                        delete=False)
        self.connector_logs = [
            tempfile.NamedTemporaryFile(mode="w+", suffix=f".connector{i + 1}", delete=False)
            for i in range(connectors)
        ]
        self._listener_final = ""
        self._connector_finals: list[str] = []

    @property
    def connector(self) -> subprocess.Popen | None:
        """The first connector, which is the only one in a session of two."""
        return self.connectors[0] if self.connectors else None

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, *_: object) -> None:
        for process in (self.listener, *self.connectors):
            if process is not None and process.poll() is None:
                process.kill()
                process.wait(timeout=10)
        # Cached before the files go, because a caller naturally reads the output *after* the
        # block that guaranteed the processes were cleaned up -- and every assertion worth
        # writing is about what they said.
        self._listener_final = self._text(self.listener_log)
        self._connector_finals = [self._text(handle) for handle in self.connector_logs]
        for handle in (self.listener_log, *self.connector_logs):
            handle.close()
            pathlib.Path(handle.name).unlink(missing_ok=True)

    def start(self, listener_extra: list[str] | None = None,
              connector_extra: list[str] | None = None) -> None:
        """Start the listener, learn its port, then start every connector."""
        expect = ["--expect", str(len(self.connector_logs) + 1)] if len(self.connector_logs) > 1 \
            else []
        self.listener = subprocess.Popen(
            [self.binary, "--headless", "--listen", *self.shared, *expect,
             *(listener_extra or [])],
            stdout=self.listener_log, stderr=subprocess.STDOUT, text=True)

        port = self._await_port()
        for log in self.connector_logs:
            self.connectors.append(subprocess.Popen(
                [self.binary, "--headless", "--connect", f"127.0.0.1:{port}", *self.shared,
                 *(connector_extra or [])],
                stdout=log, stderr=subprocess.STDOUT, text=True))

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

    def wait_all(self) -> list[int]:
        """Collect every process, and return their exit codes, the listener's first."""
        processes = [self.listener, *self.connectors]
        deadline = time.monotonic() + self.RUN_SECONDS
        codes: list[int | None] = [None] * len(processes)
        while time.monotonic() < deadline and None in codes:
            for index, process in enumerate(processes):
                if codes[index] is None and process is not None:
                    codes[index] = process.poll()
            time.sleep(0.05)
        if None in codes:
            texts = "\n".join(f"connector {i + 1}:\n{self.connector_text(i)}"
                              for i in range(len(self.connectors)))
            raise CheckFailed(
                f"a peer did not finish within {self.RUN_SECONDS}s\n"
                f"listener:\n{self.listener_text()}\n{texts}")
        return [code for code in codes if code is not None]

    def wait(self) -> tuple[int, int]:
        """Collect every process, and return the listener's and the first connector's codes."""
        codes = self.wait_all()
        return (codes[0], codes[1])

    def _text(self, handle) -> str:
        if handle.closed:
            return ""
        handle.flush()
        return pathlib.Path(handle.name).read_text(encoding="utf-8", errors="replace")

    def listener_text(self) -> str:
        return self._listener_final or self._text(self.listener_log)

    def connector_text(self, index: int = 0) -> str:
        if self._connector_finals:
            return self._connector_finals[index]
        return self._text(self.connector_logs[index])


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
