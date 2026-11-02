"""Fixtures for the end-to-end tests. See caspar.py and AGENTS.md."""

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
from caspar import Server  # noqa: E402


def _binary() -> Path:
    binary = os.environ.get("CASPAR_BINARY")
    if not binary:
        pytest.skip("CASPAR_BINARY isn't set; run through ctest, or point it at an installed casparcg")
    if not Path(binary).is_file():
        pytest.fail(f"{binary} doesn't exist; run `cmake --install <build folder>` first")
    return Path(binary)


# Errors the server may log in any test, from bugs in src/tests/known-bugs.md. Each one is pinned by a
# skipped test; remove the entry with the fix.
KNOWN_ERRORS = [
    "local_endpoint: Bad file descriptor",  # #11, pinned by test_lifecycle.py::test_kill_logs_no_errors
    "remote_endpoint: Transport endpoint is not connected",  # #13, test_amcp_tcp.py::test_early_disconnect_*
]


# Crashes (signatures in the decoded backtrace.dump) the server may have in any test, from bugs in
# src/tests/known-bugs.md. Each one is pinned by a skipped test; remove the entry with the fix.
KNOWN_CRASHES = [
    "AsyncEventServer::implementation::start_accept",  # #11, test_lifecycle.py::test_kill_logs_no_errors
]


@pytest.fixture
def output_dir(request) -> Path:
    """The test's own folder, emptied first and kept afterwards:
    $CASPAR_TEST_OUTPUT/e2e/<file>/<test>, by default ./test-output/e2e/<file>/<test>."""
    root = Path(os.environ.get("CASPAR_TEST_OUTPUT") or "test-output").absolute()
    name = re.sub(r"[^\w.-]", "_", request.node.name)
    folder = root / "e2e" / Path(request.node.fspath).stem / name
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    return folder


@pytest.fixture
def make_server(output_dir):
    """Returns a function that starts a server, by default one 720p5000 channel with no consumers:
    make_server(channels=("1080i5000",), accelerator="vulkan", extra_config="<...>", start=True).
    Every server is checked when the test ends: it is stopped with KILL if still running and must
    exit with code 0, without unexpected errors in its log or sanitizer reports."""
    servers = []

    def make(channels=("720p5000",), accelerator=None, extra_config="", start=True) -> Server:
        accelerator = accelerator or os.environ.get("CASPAR_TEST_ACCELERATOR", "auto")
        server = Server(_binary(), output_dir / f"server{len(servers)}", channels, accelerator, extra_config)
        for text in KNOWN_ERRORS:
            server.allow_errors(text)
        server.known_crashes.extend(KNOWN_CRASHES)
        servers.append(server)
        if start:
            server.start()
        return server

    yield make

    failures = []
    for server in servers:
        if server.process is None:
            continue
        try:
            server.check_shutdown()
        except AssertionError as e:
            failures.append(str(e))
    if failures:
        pytest.fail("\n\n".join(failures), pytrace=False)


@pytest.fixture
def server(make_server) -> Server:
    """A running server with one 720p5000 channel."""
    return make_server()


@pytest.fixture
def make_clip():
    """Writes a stamped test clip (see src/tests/support/test_media.h) into a folder:
    make_clip(server.media / "clip.mov", frames=50, format="720p5000")."""
    tool = os.environ.get("CASPAR_MAKE_CLIP")
    if not tool:
        pytest.skip("CASPAR_MAKE_CLIP isn't set; run through ctest")

    def make(path: Path, frames=50, format="720p5000", codec="ffv1"):
        subprocess.run(
            [tool, str(path), "--frames", str(frames), "--format", format, "--codec", codec], check=True
        )
        return path

    return make
