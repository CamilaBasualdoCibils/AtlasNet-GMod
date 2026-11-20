#!/usr/bin/env python3
"""Exercise the real AtlasNet node with a controlled gateway instead of GMod."""

from contextlib import contextmanager
from enum import IntEnum
import http.client
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parents[1]
BUILD = Path(os.getenv("ATLASNET_GMOD_BUILD_DIR", REPO / "build")).resolve()
# Matches the local SOCK_SEQPACKET record, including its reserved two bytes.
FRAME = struct.Struct("=HHIIH768s2x")


class Event(IntEnum):
    ADMIT = 0
    RESULT = 1
    CANCEL = 2
    ESTABLISHED = 3
    DISCONNECT = 4
    CHAT = 5
    SIGNAL = 6
    DELIVERED = 7
    KICK = 8


def send(gateway, event, port, token=0, value=0, text=b""):
    assert gateway.send(FRAME.pack(event, port, token, value, len(text), text)) == FRAME.size


def receive(gateway):
    event, port, token, value, size, text = FRAME.unpack(gateway.recv(FRAME.size))
    return Event(event), port, token, value, text[:size]


def bound(kind):
    sock = socket.socket(socket.AF_INET, kind)
    sock.bind(("127.0.0.1", 0))
    sock.settimeout(6)
    return sock


def wait_for(condition, label):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if condition():
            return
        time.sleep(0.05)
    raise RuntimeError(f"Timed out waiting for {label}")


@contextmanager
def node(backend, admission):
    dev = REPO / ".dev"
    dev.mkdir(exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="source1-test-", dir=dev))
    print(f"{admission} test log in {run}", flush=True)
    with bound(socket.SOCK_DGRAM) as udp, bound(socket.SOCK_STREAM) as valkey,          bound(socket.SOCK_STREAM) as metrics:
        public_port = udp.getsockname()[1]
        valkey_port = valkey.getsockname()[1]
        metrics_port = metrics.getsockname()[1]
    config = {
        "node": {
            "capabilities": ["Database", "ClientIngress", "Shard", "ControllerEligible"],
            "valkey": {"uri": f"tcp://127.0.0.1:{valkey_port}"},
            "shards": {"max_count": 1},
            "metrics": {"port": metrics_port},
            "ingress-endpoints": [{
                "id": "source1-test", "name": "source1",
                "args": {"port": public_port, "backend_port": backend.getsockname()[1],
                         "gateway_socket": "gateway.sock", "admission": admission,
                         "spawn": "GModDemo", "idle_seconds": 2},
            }],
        },
    }
    (run / "node.json").write_text(json.dumps(config))
    log_path = run / "node.log"
    with log_path.open("wb") as log, subprocess.Popen(
        [BUILD / "atlasnet/AtlasNetNode-Local", "--node-config", "node.json",
         "--cluster-config", REPO / "config/source1-cluster.yaml",
         "--module", BUILD / "atlasnet_source1.so"],
        cwd=run, stdout=log, stderr=subprocess.STDOUT,
    ) as process:
        try:
            def ready():
                assert process.poll() is None, f"AtlasNet exited. See {log_path}"
                return "Heuristic controller installed" in log_path.read_text()

            wait_for(ready, "the AtlasNet shard region")
            yield process, run / "gateway.sock", ("127.0.0.1", public_port), metrics_port
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=12)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def metric(port, name):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
    try:
        conn.request("GET", "/metrics")
        data = conn.getresponse().read().decode()
    finally:
        conn.close()
    return int(re.search(rf"^atlasnet_ingress_endpoint_{name}{{[^}}]+}} (\d+)$", data, re.M)[1])


def opened_gateway(path):
    gateway = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    gateway.settimeout(6)
    gateway.connect(str(path))
    return gateway


def relay(client, address, backend, label):
    client.sendto(label, address)
    data, routed = backend.recvfrom(1024)
    assert data == label and routed[0] == "127.0.0.2"
    backend.sendto(b"echo " + label, routed)
    assert client.recvfrom(1024) == (b"echo " + label, address)
    return routed[1]


def admit(gateway, port, serial):
    send(gateway, Event.ADMIT, port, value=serial, text=b"0")
    event, returned_port, token, request, text = receive(gateway)
    assert (event, returned_port, request) == (Event.RESULT, port, serial)
    return token, text


def chat(gateway, port, token, message):
    send(gateway, Event.CHAT, port, token, text=message)
    event, to_port, to_token, instance, text = receive(gateway)
    assert (event, to_port, to_token, text) == (Event.SIGNAL, port, token, message)
    send(gateway, Event.DELIVERED, port, token, instance, b"1")


def allow_cases():
    with bound(socket.SOCK_DGRAM) as backend, node(backend, "AllowAll") as runtime:
        process, path, address, metrics = runtime
        with opened_gateway(path) as gateway, bound(socket.SOCK_DGRAM) as first,              bound(socket.SOCK_DGRAM) as second, bound(socket.SOCK_DGRAM) as pending:
            first_port = relay(first, address, backend, b"first")
            second_port = relay(second, address, backend, b"second")
            assert first_port != second_port
            first_token, first_id = admit(gateway, first_port, 1)
            second_token, second_id = admit(gateway, second_port, 2)
            assert first_token and second_token and first_id != second_id
            send(gateway, Event.ESTABLISHED, first_port, first_token)
            send(gateway, Event.ESTABLISHED, second_port, second_token)
            chat(gateway, first_port, first_token, b"hello atlasnet")
            chat(gateway, second_port, second_token, b"hello second")
            assert metric(metrics, "packet_errors_total") == 0
            send(gateway, Event.DISCONNECT, first_port, first_token)
            wait_for(lambda: metric(metrics, "active_clients") == 1, "first client removal")
            chat(gateway, second_port, second_token, b"still connected")
            event, port, token, _, reason = receive(gateway)
            assert (event, port, token) == (Event.KICK, second_port, second_token)
            assert b"timed out" in reason
            wait_for(lambda: metric(metrics, "active_clients") == 0, "idle peer removal")

            pending_port = relay(pending, address, backend, b"cancel")
            send(gateway, Event.ADMIT, pending_port, value=3, text=b"0")
            send(gateway, Event.CANCEL, pending_port, value=3)
            # A completion that wins the race must still be disconnected.
            wait_for(lambda: metric(metrics, "cancelled_total") == 1 or
                     metric(metrics, "disconnects_total") == 3, "cancelled admission cleanup")
            assert metric(metrics, "pending_admissions") == metric(metrics, "active_clients") == 0

            with bound(socket.SOCK_DGRAM) as failed_backend_client:
                port = relay(failed_backend_client, address, backend, b"backend-loss")
                token, _ = admit(gateway, port, 4)
                assert token
                send(gateway, Event.ESTABLISHED, port, token)
                wait_for(lambda: metric(metrics, "active_clients") == 1, "client before gateway EOF")
        wait_for(lambda: metric(metrics, "active_clients") == 0, "gateway EOF removal")

        with opened_gateway(path) as gateway, bound(socket.SOCK_DGRAM) as client:
            port = relay(client, address, backend, b"shutdown")
            token, _ = admit(gateway, port, 5)
            assert token
            send(gateway, Event.ESTABLISHED, port, token)
            process.terminate()
            event, to_port, to_token, _, _ = receive(gateway)
            assert (event, to_port, to_token) == (Event.KICK, port, token)
            assert process.wait(timeout=12) == 0


def reject_case():
    with bound(socket.SOCK_DGRAM) as backend, node(backend, "GModDemoReject") as runtime:
        _, path, address, metrics = runtime
        with opened_gateway(path) as gateway, bound(socket.SOCK_DGRAM) as client:
            port = relay(client, address, backend, b"reject")
            token, reason = admit(gateway, port, 1)
            assert not token and reason == b"AtlasNet demo admission rejected"
            assert metric(metrics, "rejected_total") == 1
            assert metric(metrics, "active_clients") == 0


if __name__ == "__main__":
    allow_cases()
    reject_case()
    print("PASS real AtlasNet ingress lifecycle and gameplay routing with a controlled gateway")
