"""The AMCP TCP server under awkward clients. The server has no channels, so these tests need no
accelerator. Every test also checks, through the fixtures, that the server shuts down cleanly with no
errors in its log."""

import socket
import threading
import time

import pytest

from caspar import wait_until


@pytest.fixture
def server(make_server):
    return make_server(channels=())


def test_pipelined_commands_in_one_write(server):
    with server.amcp() as client:
        client.send_raw(b"VERSION\r\nVERSION\r\nVERSION\r\n")
        assert [client.read_reply().code for _ in range(3)] == [201, 201, 201]


def test_command_split_across_writes(server):
    with server.amcp() as client:
        for part in (b"VER", b"SI", b"ON\r", b"\n"):
            client.send_raw(part)
            time.sleep(0.05)
        assert client.read_reply().code == 201


def test_unknown_command(server):
    assert server.command("NOT_A_COMMAND").code == 400


def test_many_clients_at_once(server):
    replies = []

    def client():
        with server.amcp() as c:
            replies.append(c.send("VERSION").code)

    threads = [threading.Thread(target=client) for _ in range(50)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert replies == [201] * 50


def test_client_leaving_mid_command(server):
    for _ in range(20):
        with server.amcp() as client:
            client.send_raw(b"VERS")
    assert server.command("VERSION").code == 201


def test_client_leaving_with_a_reply_pending(server):
    for _ in range(100):
        with server.amcp() as client:
            client.send_raw(b"HELP\r\n")
    assert server.command("VERSION").code == 201


def test_garbage_and_invalid_utf8(server):
    with server.amcp() as client:
        client.send_raw(bytes(range(256)) + b"\r\n" + b"\xff\xfe\xc3\x28 PLAY\r\n")
        client.socket.settimeout(5)
        client.read_reply()
    assert server.command("VERSION").code == 201


def test_oversized_line(server):
    with server.amcp() as client:
        client.send_raw(b"A" * (1 << 20) + b"\r\n")
        client.socket.settimeout(10)
        try:
            client.read_reply()
        except (ConnectionError, OSError):
            pass  # closing the connection is a fine answer too
    assert server.command("VERSION").code == 201


def test_reconnect_storm_leaks_no_connections(server):
    with server.amcp() as client:  # settle: one connection opened and closed
        client.send("VERSION")
    assert wait_until(lambda: server.open_files() > 0)
    baseline = server.open_files()

    for _ in range(300):
        socket.create_connection(("127.0.0.1", server.port)).close()

    assert server.command("VERSION").code == 201
    assert wait_until(lambda: server.open_files() <= baseline, timeout=5), (
        f"{server.open_files()} files open, {baseline} before the storm"
    )


@pytest.mark.skip(reason="known bug #13 (known-bugs.md): a client that disconnects makes the server log "
                  "'remote_endpoint: Transport endpoint is not connected' as an error")
def test_early_disconnect_logs_no_errors(server):
    server.allowed_errors.remove("remote_endpoint: Transport endpoint is not connected")
    for _ in range(20):
        with server.amcp() as client:
            client.send_raw(b"HELP\r\n")
    assert server.command("VERSION").code == 201
