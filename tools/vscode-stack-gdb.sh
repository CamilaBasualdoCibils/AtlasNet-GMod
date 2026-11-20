#!/usr/bin/env bash
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "${GMOD_WORLD_CONFIG:-$repo/config/gmod-world.sh}"
export ATLASNET_GMOD_SPAWN_WEST_X ATLASNET_GMOD_SPAWN_EAST_X
export ATLASNET_GMOD_RUNTIME="$repo/.dev/vscode-stack"
export ATLASNET_GMOD_SHARD_LAUNCHER="$repo/tools/run-shard-gmod.py"
unset ATLASNET_GMOD_GATEWAY_SOCKET
exec /usr/bin/gdb -x "$repo/tools/vscode-stack-gdb.py" "$@"
