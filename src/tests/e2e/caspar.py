"""Runs the real CasparCG server for end-to-end tests: config, process, AMCP client and log checks.

Tests use it through the fixtures in conftest.py; see AGENTS.md.
"""

import os
import re
import signal
import socket
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path

from decode_backtrace import decode


@dataclass
class Reply:
    code: int
    status: str  # the first line, e.g. "201 VERSION OK"
    lines: list = field(default_factory=list)  # the data lines after it


class AmcpClient:
    """One AMCP connection. send() returns the parsed reply."""

    def __init__(self, port, timeout=10.0):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.buffer = b""

    def close(self):
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def send_raw(self, data: bytes):
        self.socket.sendall(data)

    def read_line(self) -> str:
        while b"\r\n" not in self.buffer:
            chunk = self.socket.recv(65536)
            if not chunk:
                raise ConnectionError("the server closed the connection")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\r\n", 1)
        return line.decode("utf-8", errors="replace")

    def read_reply(self) -> Reply:
        status = self.read_line()
        code = int(status.split(" ", 1)[0])
        reply = Reply(code, status)
        if code == 200:  # data lines up to an empty one
            while (line := self.read_line()) != "":
                reply.lines.append(line)
        elif code in (201, 400):  # one data line
            reply.lines.append(self.read_line())
        return reply

    def send(self, command: str) -> Reply:
        self.send_raw(command.encode("utf-8") + b"\r\n")
        return self.read_reply()


def wait_until(done, timeout=5.0, interval=0.05) -> bool:
    """Polls `done()` until it returns true or `timeout` seconds pass."""
    deadline = time.monotonic() + timeout
    while not done():
        if time.monotonic() > deadline:
            return False
        time.sleep(interval)
    return True


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# Log lines and output that mean something went wrong, wherever they appear.
SANITIZER_REPORT = re.compile(r"ERROR: (Address|Leak|Thread)Sanitizer|runtime error:")
ERROR_LINE = re.compile(r"\[(error|fatal)\]")


@dataclass
class Exit:
    code: int  # the exit code, or -signal when killed by a signal

    @property
    def signal_name(self):
        return signal.Signals(-self.code).name if self.code < 0 else None

    def __str__(self):
        return f"killed by {self.signal_name}" if self.code < 0 else f"exit code {self.code}"


class Server:
    """A casparcg process with its own config, folders and AMCP port, all under `folder`."""

    def __init__(self, binary: Path, folder: Path, channels=("720p5000",), accelerator="auto", extra_config=""):
        self.binary = binary
        self.folder = folder
        self.port = free_port()
        self.process = None
        self.exit = None
        self.expected_exit = 0  # what check_shutdown() expects; a test can change it
        # Crashes whose decoded stack contains one of these are known bugs and don't fail the test.
        self.known_crashes = []
        # Known bug #14 (known-bugs.md): the process can abort after a complete shutdown.
        self.allow_abort_after_shutdown = True
        self.allowed_errors = []
        self.media = folder / "media"
        for sub in ("media", "log", "data", "template"):
            (folder / sub).mkdir(parents=True, exist_ok=True)

        channels_xml = "".join(
            f"<channel><video-mode>{mode}</video-mode><consumers /></channel>" for mode in channels
        )
        self.config = folder / "casparcg.config"
        self.config.write_text(
            f"""<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <paths>
    <media-path>{folder}/media/</media-path>
    <log-path disabled="true" />
    <data-path>{folder}/data/</data-path>
    <template-path>{folder}/template/</template-path>
  </paths>
  <log-level>debug</log-level>
  <accelerator>{accelerator}</accelerator>
  <channels>{channels_xml}</channels>
  <controllers><tcp><port>{self.port}</port><protocol>AMCP</protocol></tcp></controllers>
  {extra_config}
</configuration>
"""
        )
        self.log_path = folder / "server.log"

    # Process

    def start(self, timeout=30.0, expect_ready=True):
        """Starts the server and, unless told otherwise, waits until AMCP answers VERSION."""
        self.log_file = open(self.log_path, "wb")
        self.process = subprocess.Popen(
            [str(self.binary), str(self.config)],
            cwd=self.folder,
            stdin=subprocess.PIPE,
            stdout=self.log_file,
            stderr=subprocess.STDOUT,
        )
        if not expect_ready:
            return
        deadline = time.monotonic() + timeout
        while True:
            if self.process.poll() is not None:
                self.wait_exit()
                raise AssertionError(f"the server exited ({self.exit}) before answering AMCP\n{self.tail()}")
            try:
                with self.amcp(timeout=2.0) as client:
                    if client.send("VERSION").code == 201:
                        return
            except OSError:
                pass
            if time.monotonic() > deadline:
                raise AssertionError(f"AMCP didn't answer within {timeout} s\n{self.tail()}")
            time.sleep(0.1)

    @property
    def running(self):
        return self.process is not None and self.process.poll() is None

    def console(self, text: str):
        """Types a line into the server's console (stdin)."""
        self.process.stdin.write(text.encode() + b"\n")
        self.process.stdin.flush()

    def signal(self, signum):
        self.process.send_signal(signum)

    def wait_exit(self, timeout=15.0) -> Exit:
        """Waits for the process to end. If it doesn't, dumps its threads' stacks and kills it."""
        try:
            code = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            stacks = self.dump_stacks()
            self.process.kill()
            self.process.wait()
            self.log_file.close()
            raise AssertionError(f"the server didn't exit within {timeout} s; thread stacks: {stacks}\n{self.tail()}")
        self.log_file.close()
        self.exit = Exit(code)
        return self.exit

    def dump_stacks(self) -> Path:
        path = self.folder / "stacks.txt"
        with open(path, "w") as out:
            subprocess.run(
                ["gdb", "-q", "-batch", "-p", str(self.process.pid), "-ex", "thread apply all bt"],
                stdout=out,
                stderr=subprocess.STDOUT,
                timeout=120,
            )
        return path

    def open_files(self) -> int:
        """How many file descriptors the server has open (Linux)."""
        return len(os.listdir(f"/proc/{self.process.pid}/fd"))

    # AMCP

    def amcp(self, timeout=10.0) -> AmcpClient:
        return AmcpClient(self.port, timeout)

    def command(self, command: str) -> Reply:
        """Sends one command on a connection of its own."""
        with self.amcp() as client:
            return client.send(command)

    # Log

    def log(self) -> str:
        return self.log_path.read_text(errors="replace")

    def tail(self, lines=30) -> str:
        return "--- last lines of " + str(self.log_path) + "\n" + "\n".join(self.log().splitlines()[-lines:])

    def allow_errors(self, text: str):
        """Error lines containing `text` don't fail the test."""
        self.allowed_errors.append(text)

    def unexpected_errors(self) -> list:
        """Error and fatal records in the log that weren't allowed. A record is a run of error lines
        (an exception's details come on lines of their own); an allowance matches anywhere in it."""
        records = []
        previous_was_error = False
        for line in self.log().splitlines():
            is_error = bool(ERROR_LINE.search(line))
            if is_error and previous_was_error:
                records[-1] += "\n" + line
            elif is_error:
                records.append(line)
            previous_was_error = is_error
        return [r for r in records if not any(text in r for text in self.allowed_errors)]

    def crash_stack(self) -> str:
        """The decoded backtrace.dump the server's crash handler wrote, or "" if it didn't crash."""
        dump = self.folder / "backtrace.dump"
        return decode(self.binary, dump) if dump.exists() else ""

    def sanitizer_reports(self) -> list:
        return [line for line in self.log().splitlines() if SANITIZER_REPORT.search(line)]

    # Teardown

    def check_shutdown(self):
        """Stops the server if it is still running (KILL), then fails on an exit code other than
        expected_exit, a signal, an unexpected error in the log or a sanitizer report."""
        if self.running:
            try:
                self.command("KILL")
            except OSError:
                self.process.terminate()
        if self.exit is None and self.process is not None:
            self.wait_exit()

        problems = []
        aborted_after_shutdown = (
            self.exit is not None
            and self.exit.signal_name == "SIGABRT"
            and "Successfully shutdown CasparCG Server." in self.log()
        )
        stack = self.crash_stack()
        known_crash = stack and any(signature in stack for signature in self.known_crashes)
        if (
            self.exit is not None
            and self.exit.code != self.expected_exit
            and not (aborted_after_shutdown and self.allow_abort_after_shutdown)
            and not known_crash
        ):
            problems.append(f"the server ended with {self.exit}")
            if stack:
                problems.append("crash stack (backtrace.dump):\n" + stack)
        if errors := self.unexpected_errors():
            problems.append("errors in the log:\n" + "\n".join(errors[:20]))
        if reports := self.sanitizer_reports():
            problems.append("sanitizer reports:\n" + "\n".join(reports[:20]))
        if problems:
            raise AssertionError("\n".join(problems) + f"\nlog: {self.log_path}")
