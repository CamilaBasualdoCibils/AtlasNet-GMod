#!/usr/bin/env python3
"""Launch a loopback AtlasNet Source 1 demo with a dedicated server."""

import argparse
import os
from pathlib import Path
import shlex
import signal
import socket
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parents[1]


def wait_for(process, ready, description, seconds=45):
    deadline = time.monotonic() + seconds
    while not ready():
        if (status := process.poll()) is not None:
            raise RuntimeError(f"{description} exited with status {status}")
        if time.monotonic() >= deadline:
            raise RuntimeError(f"Timed out waiting for {description}")
        time.sleep(0.15)


def quit_process(process, write_quit=False):
    if process is None or process.poll() is not None:
        return
    try:
        if write_quit:
            process.stdin.write(b"quit\n")
        else:
            process.terminate()
        process.wait(timeout=12)
    except (BrokenPipeError, subprocess.TimeoutExpired):
        process.kill()
        process.wait()


def run_stack(build, working_dir, configuration):
    gateway = working_dir / "gateway.sock"
    node = server = None
    with (working_dir / "node.log").open("wb") as node_log, (working_dir / "gmod.log").open("wb") as server_log:
        try:
            node = subprocess.Popen(
                [build / "atlasnet/AtlasNetNode-Local", "--node-config", configuration,
                 "--cluster-config", REPO / "config/source1-cluster.yaml",
                 "--module", build / "atlasnet_source1.so",
                 "--valkey-uri", f"tcp://127.0.0.1:{valkey_port()}"],
                cwd=working_dir, stdout=node_log, stderr=subprocess.STDOUT, start_new_session=True,
            )
            wait_for(node, gateway.is_socket, "AtlasNet endpoint")
            wait_for(node, lambda: "Heuristic controller installed" in
                     (working_dir / "node.log").read_text(errors="replace"), "AtlasNet spawn region")
            env = dict(os.environ, GMOD_DEV_PORT="27016", ATLASNET_GMOD_GATEWAY_SOCKET=str(gateway))
            command = shlex.join([str(REPO / "tools/run-dev-server.sh"), "-noconclr", "-noworkshop"])
            server = subprocess.Popen(
                ["script", "-qefc", command, "/dev/null"], cwd=REPO, env=env,
                stdin=subprocess.PIPE, bufsize=0, stdout=server_log, stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            wait_for(server, lambda: "Dedicated server gateway attached" in
                     (working_dir / "node.log").read_text(errors="replace"), "GMod gateway")
            if "ports 27016 SV" not in (working_dir / "gmod.log").read_text(errors="replace"):
                raise RuntimeError("GMod failed to bind the requested backend port 27016")
            print("AtlasNet Source ingress listening at 127.0.0.1:27015", flush=True)

            while node.poll() is None and server.poll() is None:
                time.sleep(0.25)
            raise RuntimeError(f"Stack process exited (AtlasNet={node.poll()}, GMod={server.poll()})")
        finally:
            # Stop admission while the GMod gateway can still deliver kicks.
            quit_process(node)
            quit_process(server, write_quit=True)


def valkey_port():
    # Keep the demo off any existing local Valkey data.
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        return reservation.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=REPO / "config/source1-dev.yaml")
    parser.add_argument("--skip-install", action="store_true", help="Use existing build and server files")
    args = parser.parse_args()
    build = Path(os.getenv("ATLASNET_GMOD_BUILD_DIR", REPO / "build")).resolve()
    if not args.skip_install:
        subprocess.run(["cmake", "--build", build, "--target",
                        "atlasnet_source1", "AtlasNetNode-Local", "AtlasNetShardWorker"],
                       cwd=REPO, check=True)
        subprocess.run([REPO / "tools/install-dev.sh"], cwd=REPO, check=True)
    dev = REPO / ".dev"
    dev.mkdir(exist_ok=True)
    working_dir = Path(tempfile.mkdtemp(prefix="source1-", dir=dev))
    print(f"Source 1 stack logs in {working_dir}", flush=True)

    def stop(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop)
    try:
        run_stack(build, working_dir, args.config.resolve())
    except KeyboardInterrupt:
        return 0
    except (OSError, RuntimeError) as error:
        parser.exit(1, f"{error}. See {working_dir}\n")


if __name__ == "__main__":
    main()
