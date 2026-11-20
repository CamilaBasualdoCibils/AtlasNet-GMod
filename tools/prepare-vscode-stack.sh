#!/usr/bin/env bash
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd -- "$repo"
runtime="$repo/.dev/vscode-stack"
base_server=${GMOD_SERVER_DIR:-"$repo/.dev/gmod-server"}

if [[ ! -f build/CMakeCache.txt ]]; then
    cmake --preset linux
fi
cmake --build build --target AtlasNetNode AtlasNetShardWorker AtlasNetXAxisSplit Valkey-Server-Compile atlasnet_source1
GMOD_SERVER_DIR="$base_server" tools/install-dev.sh

mkdir -p -- "$runtime/gateway-1-server" "$runtime/gateway-2-server"
# A debugger force-stop can leave a Unix socket pathname without a listener.
python3 - "$runtime" <<'PY'
import errno
from pathlib import Path
import socket
import stat
import sys

for address in Path(sys.argv[1]).glob("gateway-*.sock"):
    if not stat.S_ISSOCK(address.lstat().st_mode):
        raise SystemExit(f"Cannot replace non-socket path {address}")
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as connection:
        result = connection.connect_ex(str(address))
    if result == errno.ECONNREFUSED:
        address.unlink(missing_ok=True)
    elif result != errno.ENOENT:
        raise SystemExit(f"Gateway socket is in use or unavailable at {address}")
PY
for instance in 1 2; do
    cp -a --reflink=auto -- "$base_server/." "$runtime/gateway-$instance-server/"
done
