"""Start the development gateways alongside nodes launched by AtlasNet Local Stack."""
import json
import os
from pathlib import Path
import shlex
import sys
import time


def serve_gateway(pid, address, port, server, log):
    import select
    import signal
    import subprocess

    repo = Path(__file__).resolve().parents[1]
    with os.fdopen(os.pidfd_open(pid), "rb") as node:
        stopped = select.poll()
        stopped.register(node, select.POLLIN)
        deadline = time.monotonic() + 45
        while not address.is_socket():
            if stopped.poll(100):
                return
            if time.monotonic() >= deadline:
                raise RuntimeError(f"AtlasNet did not open {address}")
        environment = dict(os.environ, GMOD_SERVER_DIR=str(server), GMOD_DEV_PORT=str(port),
                           ATLASNET_GMOD_GATEWAY_SOCKET=str(address), ATLASNET_GMOD_DISTRIBUTED="1")
        command = shlex.join([str(repo / "tools/run-dev-server.sh"), "-noconclr", "-noworkshop"])
        game = subprocess.Popen(["script", "-qefc", command, "/dev/null"], env=environment,
                                stdin=subprocess.PIPE, start_new_session=True)
        try:
            while game.poll() is None and not stopped.poll(250):
                pass
            if game.poll() not in (None, 0):
                raise RuntimeError(f"GMod gateway exited. See {log}")
        finally:
            if game.poll() is None:
                try:
                    game.communicate(b"quit\n", timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(game.pid, signal.SIGKILL)
                    game.wait()


def install_launch_hook():
    import gdb

    class StartGateway(gdb.Breakpoint):
        def stop(self):
            program = gdb.current_progspace().filename
            if not program or Path(program).name != "AtlasNetNode":
                return False
            pid = gdb.selected_inferior().pid
            arguments = (Path("/proc") / str(pid) / "cmdline").read_bytes().decode().split("\0")
            config_path = Path(arguments[arguments.index("--node-config") + 1])
            index = int(config_path.stem.removeprefix("node-"))
            config = json.loads(config_path.read_text())["node"]
            endpoint = next((e["args"] for e in config.get("ingress-endpoints", [])
                             if e["name"] == "source1" and e["args"].get("simulation")), None)
            if endpoint is None:
                return False

            wrapper = shlex.split(gdb.parameter("exec-wrapper"))
            log_directory = Path(wrapper[4] if Path(wrapper[1]).name == "perf-wrapper.sh" else wrapper[2]).parent
            if index != 0:
                controller_log = log_directory / "node-1.log"
                deadline = time.monotonic() + 20
                while not controller_log.exists() or "Controller started" not in controller_log.read_text(errors="replace"):
                    if time.monotonic() >= deadline:
                        gdb.write(f"Controller startup timed out. See {controller_log}\n", gdb.STDERR)
                        return True
                    time.sleep(0.1)

            # A shard hosts its own Source subprocess and uses shell helpers for
            # staging. Keep those forks running independently of the debugger.
            gdb.execute("set detach-on-fork on", to_string=True)
            environment = dict(os.environ)
            for value in (Path("/proc") / str(pid) / "environ").read_bytes().split(b"\0"):
                if value.startswith(b"ATLASNET_LOCAL_STACK_SESSION="):
                    environment["ATLASNET_LOCAL_STACK_SESSION"] = os.fsdecode(value.split(b"=", 1)[1])
            runtime = Path(environment["ATLASNET_GMOD_RUNTIME"])
            log = log_directory / f"gateway-{index + 1}.log"
            helper = Path(environment["ATLASNET_GMOD_SHARD_LAUNCHER"]).with_name("vscode-stack-gdb.py")
            command = ["python3", str(helper), str(pid), endpoint["gateway_socket"],
                       str(endpoint["backend_port"]), str(runtime / f"gateway-{index + 1}-server"),
                       str(log)]
            files = [
                (os.POSIX_SPAWN_OPEN, 0, "/dev/null", os.O_RDONLY, 0),
                (os.POSIX_SPAWN_OPEN, 1, str(log), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600),
                (os.POSIX_SPAWN_DUP2, 1, 2),
            ]
            os.posix_spawnp(command[0], command, environment, file_actions=files)
            gdb.write(f"GMod gateway log: {log}\n")
            return False

    gdb.execute("set breakpoint pending on", to_string=True)
    StartGateway("main", internal=True)


if len(sys.argv) == 6:
    serve_gateway(int(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3]), Path(sys.argv[4]), Path(sys.argv[5]))
else:
    install_launch_hook()
