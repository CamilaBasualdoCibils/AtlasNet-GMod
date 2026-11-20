#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/run-gmod-client.sh [HOST[:PORT]] [-- SOURCE_FLAGS...]

Launch a fresh Garry's Mod client under Proton using the installed Steam game.
Keep Steam open and signed in. Run the script again for another client.

The default endpoint is 127.0.0.1:27215. Session files and the Source console
log are kept under .dev/gmod-clients.

Optional environment overrides:
  GMOD_STEAM_ROOT   Steam installation directory
  GMOD_GAME_DIR     Garry's Mod installation containing gmod_win64.exe
  GMOD_PROTON_DIR   Proton installation containing the proton script
EOF
}

case ${1:-} in
    -h|--help) usage; exit 0 ;;
esac

endpoint=127.0.0.1:27215
if [[ $# -gt 0 && $1 != -- ]]; then
    endpoint=$1
    shift
fi
if [[ $# -gt 0 ]]; then
    if [[ $1 != -- ]]; then
        usage >&2
        exit 2
    fi
    shift
fi
if [[ ! $endpoint =~ ^[[:alnum:]_.-]+(:[0-9]{1,5})?$ ]]; then
    printf 'Invalid endpoint %s. Use hostname or hostname:port.\n' "$endpoint" >&2
    exit 2
fi
if [[ $endpoint == *:* ]]; then
    port=${endpoint##*:}
    if (( 10#$port < 1 || 10#$port > 65535 )); then
        printf 'Endpoint port must be between 1 and 65535.\n' >&2
        exit 2
    fi
fi

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
steam_dir=${GMOD_STEAM_ROOT:-"$HOME/.steam/root"}
if [[ ! -d $steam_dir && -z ${GMOD_STEAM_ROOT:-} ]]; then
    steam_dir="$HOME/.local/share/Steam"
fi
if [[ ! -d $steam_dir ]]; then
    printf 'Steam was not found. Set GMOD_STEAM_ROOT to its installation directory.\n' >&2
    exit 1
fi
steam_dir=$(realpath -- "$steam_dir")

libraries=("$steam_dir")
library_file="$steam_dir/steamapps/libraryfolders.vdf"
if [[ -r $library_file ]]; then
    mapfile -t -O "${#libraries[@]}" libraries < <(awk -F '"' '$2 == "path" {print $4}' "$library_file")
fi

game_dir=${GMOD_GAME_DIR:-}
if [[ -z $game_dir ]]; then
    for library in "${libraries[@]}"; do
        candidate="$library/steamapps/common/GarrysMod"
        if [[ -f $candidate/gmod_win64.exe ]]; then
            game_dir=$candidate
            break
        fi
    done
fi
if [[ -z $game_dir || ! -f $game_dir/gmod_win64.exe ]]; then
    printf 'The Windows x64 Steam install of Garry’s Mod was not found. Set GMOD_GAME_DIR to its directory.\n' >&2
    exit 1
fi

proton_dir=${GMOD_PROTON_DIR:-}
if [[ -z $proton_dir ]]; then
    for library in "${libraries[@]}"; do
        candidate="$library/steamapps/common/Proton - Experimental"
        if [[ -x $candidate/proton ]]; then
            proton_dir=$candidate
            break
        fi
    done
fi
if [[ -z $proton_dir ]]; then
    for library in "${libraries[@]}"; do
        for candidate in "$library"/steamapps/common/Proton* "$library"/compatibilitytools.d/*; do
            if [[ -x $candidate/proton ]]; then
                proton_dir=$candidate
                break 2
            fi
        done
    done
fi
if [[ -z $proton_dir || ! -x $proton_dir/proton ]]; then
    printf 'Proton was not found. Install Proton in Steam or set GMOD_PROTON_DIR.\n' >&2
    exit 1
fi
game_dir=$(realpath -- "$game_dir")
proton_dir=$(realpath -- "$proton_dir")
if ! command -v rsync >/dev/null; then
    printf 'Install rsync to stage the client session.\n' >&2
    exit 1
fi

mkdir -p -- "$repo/.dev/gmod-clients"
session=$(mktemp -d "$repo/.dev/gmod-clients/client-XXXXXX")
mkdir -p -- "$session/game" "$session/compatdata" "$session/cache"
printf 'Starting GMod for %s\nSession files: %s\n' "$endpoint" "$session"

# Share the Steam content archives while keeping writable game files separate.
rsync -a --exclude='*.vpk' -- "$game_dir/" "$session/game/"
while IFS= read -r -d '' archive; do
    ln -s -- "$archive" "$session/game/${archive#"$game_dir/"}"
done < <(find "$game_dir" -type f -name '*.vpk' -print0)

cd -- "$session/game"
export SteamAppId=4000 SteamGameId=4000 STEAM_COMPAT_APP_ID=4000
export STEAM_COMPAT_DATA_PATH="$session/compatdata"
export STEAM_COMPAT_INSTALL_PATH="$PWD" STEAM_COMPAT_CLIENT_INSTALL_PATH="$steam_dir"
export XDG_CACHE_HOME="$session/cache"
exec "$proton_dir/proton" run ./gmod_win64.exe -multirun -console -condebug -conclearlog \
    -windowed -w 960 -h 640 "$@" +net_usesocketsforloopback 1 +connect "$endpoint"
