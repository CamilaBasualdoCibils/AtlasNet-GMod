#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
server_dir=${GMOD_SERVER_DIR:-"$repo_dir/.dev/gmod-server"}
source "${GMOD_WORLD_CONFIG:-$repo_dir/config/gmod-world.sh}"
export ATLASNET_GMOD_ORIGIN_X ATLASNET_GMOD_ORIGIN_Y ATLASNET_GMOD_ORIGIN_Z
if [[ ! -x "$server_dir/srcds_run_x64" || ! -x "$server_dir/bin/linux64/srcds" ]]; then
    printf 'Install the x86-64 dedicated server with tools/install-dev.sh first.\n' >&2
    exit 1
fi

cd -- "$server_dir"
exec ./srcds_run_x64 -norestart -console -game garrysmod -ip 127.0.0.1 \
    -port "${GMOD_DEV_PORT:-27015}" +sv_lan 1 +maxplayers 8 \
    +gamemode "$GMOD_GAMEMODE" +map "$GMOD_MAP" "$@"
