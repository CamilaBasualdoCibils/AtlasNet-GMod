#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
server_dir=$(realpath -m -- "${GMOD_SERVER_DIR:-$repo_dir/.dev/gmod-server}")
build_dir=${ATLASNET_GMOD_BUILD_DIR:-"$repo_dir/build"}

case "${1:-}" in
    '') ;;
    --update-server|--server-only) ;;
    *) printf 'Usage: %s [--update-server|--server-only]\n' "$0" >&2; exit 2 ;;
esac
if (( $# > 1 )); then
    printf 'Supply at most one option.\n' >&2
    exit 2
fi

if [[ -n ${1:-} || ! -x "$server_dir/srcds_run_x64" || ! -x "$server_dir/bin/linux64/srcds" ]]; then
    steamcmd=${STEAMCMD:-steamcmd}
    if ! command -v -- "$steamcmd" >/dev/null; then
        printf 'Install SteamCMD or set STEAMCMD to its executable path.\n' >&2
        exit 1
    fi

    mkdir -p -- "$server_dir"
    steam_args=(+@sSteamCmdForcePlatformType linux +force_install_dir "$server_dir"
        +login anonymous +app_update 4020 -beta x86-64 +quit)
    "$steamcmd" "${steam_args[@]}" || {
        status=$?
        if (( status != 8 )); then
            exit "$status"
        fi
        # SteamCMD can fail the first attempt while caching app metadata.
        printf 'Retrying the GMod dedicated server install/update.\n' >&2
        "$steamcmd" "${steam_args[@]}"
    }
fi

if [[ ! -x "$server_dir/srcds_run_x64" || ! -x "$server_dir/bin/linux64/srcds" ]]; then
    printf 'SteamCMD did not install the x86-64 server in %s.\n' "$server_dir" >&2
    exit 1
fi
if [[ ${1:-} == --server-only ]]; then
    exit 0
fi

cmake --build "$build_dir" --target atlasnet_gmod
module_dir="$server_dir/garrysmod/lua/bin"
autorun_dir="$server_dir/garrysmod/addons/atlasnet_gmod_dev/lua/autorun/server"
install -Dm755 -- "$build_dir/gmsv_atlasnet_linux64.dll" "$module_dir/gmsv_atlasnet_linux64.dll"
install -Dm644 -- "$repo_dir/lua/autorun/server/atlasnet.lua" "$autorun_dir/atlasnet.lua"
install -Dm644 -- "$repo_dir/lua/autorun/server/atlasnet_gateway.lua" "$autorun_dir/atlasnet_gateway.lua"
install -Dm644 -- "$repo_dir/lua/autorun/server/atlasnet_simulation.lua" "$autorun_dir/atlasnet_simulation.lua"
install -Dm644 -- "$repo_dir/lua/entities/atlasnet_replica.lua" \
    "$server_dir/garrysmod/addons/atlasnet_gmod_dev/lua/entities/atlasnet_replica.lua"
printf 'Installed AtlasNet-GMod in %s\n' "$server_dir"
