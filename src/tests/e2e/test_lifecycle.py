"""Starting and stopping the server. The fixtures check every server's exit: code 0 (or the test's
expected_exit), no unexpected errors in its log and no sanitizer reports, so tests only wait for it."""

import signal
import socket
import time

import pytest


def test_starts_and_answers_version(server):
    reply = server.command("VERSION")
    assert reply.code == 201
    assert reply.lines[0].startswith("2.")


def test_kill_shuts_down(server):
    assert server.command("KILL").code == 202
    server.wait_exit()


@pytest.mark.skip(reason="known bug #11 (known-bugs.md): shutdown can log 'local_endpoint: Bad file descriptor'")
def test_kill_logs_no_errors(server):
    server.allowed_errors.clear()
    server.known_crashes.clear()
    assert server.command("KILL").code == 202
    assert server.wait_exit().code == 0


@pytest.mark.skip(
    reason="known bug #14 (known-bugs.md): the server can abort after 'Successfully shutdown', "
    "in about 1 of 8 KILLs"
)
def test_repeated_kill_never_aborts(make_server):
    for _ in range(20):
        server = make_server(channels=())
        server.allow_abort_after_shutdown = False
        assert server.command("KILL").code == 202
        assert server.wait_exit().code == 0


def test_restart_exits_with_code_5(server):
    server.expected_exit = 5  # tells the service wrapper to start it again
    assert server.command("RESTART").code == 202
    assert server.wait_exit().code == 5


@pytest.mark.parametrize("command", ["q", "EXIT", "BYE"])
def test_console_quit_shuts_down(server, command):
    server.console(command)
    server.wait_exit()


@pytest.mark.parametrize("signum", [signal.SIGINT, signal.SIGTERM])
def test_signal_shuts_down(server, signum):
    server.signal(signum)
    server.wait_exit()


@pytest.mark.skip(
    reason="known bug #5 (known-bugs.md): the ffmpeg producer's teardown outlives main(); the server aborts "
    "at exit in about 1 of 3 runs"
)
def test_shuts_down_while_playing(server, make_clip):
    make_clip(server.media / "clip.mov", frames=50)
    assert server.command("PLAY 1-10 clip LOOP").code == 202
    time.sleep(2)
    assert server.command("KILL").code == 202
    assert server.wait_exit().code == 0


@pytest.mark.skip(
    reason="known bug #5 (known-bugs.md): the ffmpeg producer's teardown outlives main(); the server aborts "
    "at exit in about 1 of 3 runs"
)
def test_kill_right_after_clear(server, make_clip):
    make_clip(server.media / "clip.mov", frames=50)
    assert server.command("PLAY 1-10 clip LOOP").code == 202
    time.sleep(1)
    with server.amcp() as client:
        assert client.send("CLEAR 1").code == 202
        assert client.send("KILL").code == 202
    assert server.wait_exit().code == 0


def start_with_port_taken(make_server):
    server = make_server(start=False)
    server.allow_errors("already in use")
    with socket.socket() as blocker:
        blocker.bind(("127.0.0.1", server.port))
        blocker.listen()
        server.start(expect_ready=False)
        server.wait_exit(timeout=30)
    return server


def test_port_in_use_is_reported(make_server):
    server = start_with_port_taken(make_server)
    server.expected_exit = server.exit.code  # see test_failed_startup_exits_with_an_error
    assert "It is likely already in use" in server.log()


@pytest.mark.skip(reason="known bug #12 (known-bugs.md): a server that fails to start exits with code 0")
def test_failed_startup_exits_with_an_error(make_server):
    server = start_with_port_taken(make_server)
    server.expected_exit = server.exit.code
    assert server.exit.code != 0
