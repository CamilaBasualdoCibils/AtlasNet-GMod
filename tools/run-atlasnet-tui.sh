#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
program=${ATLASNET_TUI_BIN:-"$repo_dir/../AtlasNet-TUI/build/atlasnet-tui"}
if [[ ! -x "$program" ]]; then
    printf 'Set ATLASNET_TUI_BIN to the AtlasNet-TUI executable. Looked in %s\n' "$program" >&2
    exit 1
fi

url=$(python3 - "$repo_dir/.vscode/atlasnet-stack.json" <<'PY'
import json
import sys
from pathlib import Path

settings = json.loads(Path(sys.argv[1]).read_text())
index = 0
for group in settings["groups"]:
    if "ControlPanel" in group["capabilities"]:
        panel = group.get("nodeSettings", {}).get("control_panel", {})
        address = panel.get("address", "127.0.0.1")
        if address == "0.0.0.0":
            address = "127.0.0.1"
        port = panel.get("port", settings["controlPanelPort"] + index)
        print(f"ws://{address}:{port}")
        break
    index += group["count"]
else:
    sys.exit("Enable ControlPanel for a node in AtlasNet Local Stack first.")
PY
)
exec "$program" --connect "$url" "$@"
