#!/usr/bin/env python3
"""Start the game-thread half of an SDK shard worker."""
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parents[1]
shard, fd = sys.argv[1:]
server = Path(os.environ.get("GMOD_SERVER_DIR", repo / ".dev/gmod-server")).resolve()
run = Path(tempfile.mkdtemp(prefix="sim-", dir=os.environ["ATLASNET_GMOD_RUNTIME"]))
game = run / "server"
# Keep the inherited simulation channel away from helper processes.
os.set_inheritable(int(fd), False)
subprocess.run(["cp", "-a", "--reflink=auto", str(server), str(game)], check=True)
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
    udp.bind(("127.0.0.1", 0))
    port = udp.getsockname()[1]
os.set_inheritable(int(fd), True)
output = os.open(run / "gmod.log", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
os.dup2(output, 1)
os.dup2(output, 2)
os.close(output)
print(f"AtlasNet shard {shard} game port {port}", flush=True)
env = dict(os.environ, GMOD_SERVER_DIR=str(game), GMOD_DEV_PORT=str(port),
           ATLASNET_GMOD_SIM_FD=fd)
env.pop("ATLASNET_GMOD_GATEWAY_SOCKET", None)
launcher = repo / "tools/run-dev-server.sh"
os.execve(launcher, [str(launcher), "-allowquit", "-noconclr", "-noworkshop",
                     "+sv_hibernate_think", "1"], env)
