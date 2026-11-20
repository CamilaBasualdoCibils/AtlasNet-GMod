#!/usr/bin/env python3
"""Check a real x86-64 GMod server and preserve its console log."""

import argparse
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[1]
INITIALIZED = "[AtlasNet-GMod] Native module initialized"
SHUT_DOWN = "[AtlasNet-GMod] Native module shut down"


def wait_for_message(server, log, message, timeout):
    deadline = time.monotonic() + timeout
    console = b""
    while True:
        console += log.read()
        if message.encode() in console:
            return
        if server.poll() is not None:
            raise RuntimeError(f"Server exited with status {server.returncode} while waiting for {message}")
        if time.monotonic() >= deadline:
            raise RuntimeError(f"Timed out waiting for {message}")
        time.sleep(0.1)


def smoke(baseline, log_path, timeout):
    args = ["-noconclr", "-noworkshop"]
    map_check = 'game.GetMap()=="gm_construct" and engine.ActiveGamemode()=="sandbox"'
    if baseline:
        args.append("-noaddons")
        map_check += " and atlasnet == nil"

    # Source's Linux console needs a controlling terminal. util-linux script
    # supplies one while forwarding the commands and preserving the exit status.
    command = shlex.join([str(REPO / "tools/run-dev-server.sh"), *args])
    with (
        log_path.open("wb") as output,
        log_path.open("rb") as log,
        subprocess.Popen(
            ["script", "-qefc", command, "/dev/null"],
            cwd=REPO,
            stdin=subprocess.PIPE,
            stdout=output,
            stderr=subprocess.STDOUT,
            bufsize=0,
        ) as server,
    ):
        try:
            wait_for_message(server, log, "Initializing Steam libraries for LAN server", timeout)
            map_probe = f'lua_run assert({map_check}) print("[AtlasNet smoke] " .. "map ready")\n'
            server.stdin.write(map_probe.encode())
            wait_for_message(server, log, "[AtlasNet smoke] map ready", timeout)

            if not baseline:
                server.stdin.write(
                    b'lua_run assert(atlasnet.IsInitialized()==true) '
                    b'print("[AtlasNet smoke] " .. "diagnostics ready",atlasnet.Version())\n'
                )
                wait_for_message(server, log, "[AtlasNet smoke] diagnostics ready", timeout)
                server.stdin.write(
                    b'lua_run local ok,v,i=coroutine.resume(coroutine.create(function() '
                    b'return atlasnet.Version(),atlasnet.IsInitialized() end)) '
                    b'assert(ok and i==true) print("[AtlasNet smoke] " .. "coroutine ready",v)\n'
                )
                wait_for_message(server, log, "[AtlasNet smoke] coroutine ready", timeout)

            server.stdin.write(b"quit\n")
            if server.wait(timeout=20) != 0:
                raise RuntimeError(f"Server exited with status {server.returncode} after quit")
        finally:
            if server.poll() is None:
                server.terminate()
                server.wait()

    messages = log_path.read_text(errors="replace")
    if baseline:
        if INITIALIZED in messages or SHUT_DOWN in messages:
            raise RuntimeError("AtlasNet loaded during the baseline test")
    elif messages.count(INITIALIZED) != 1 or messages.count(SHUT_DOWN) != 1:
        raise RuntimeError("Expected one native initialization and one native shutdown")
    else:
        result = re.search(r"\[AtlasNet smoke\] diagnostics ready\s+(AtlasNet-GMod \d+\.\d+\.\d+)\s", messages)
        if result is None or f"[AtlasNet-GMod] {result[1]} | initialized=true" not in messages:
            raise RuntimeError("The Lua version or initialization report was missing")
        if f"[AtlasNet smoke] coroutine ready\t{result[1]}" not in messages:
            raise RuntimeError("The coroutine returned a different version")
    print(f"PASS {'baseline' if baseline else 'AtlasNet module'} ({log_path})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", action="store_true", help="Boot with addons disabled")
    parser.add_argument("--timeout", type=float, default=30, help="Seconds to wait for each console check")
    parser.add_argument("--log", type=Path)
    args = parser.parse_args()
    log = args.log or REPO / ".dev" / ("baseline.log" if args.baseline else "atlasnet-smoke.log")
    log.parent.mkdir(parents=True, exist_ok=True)
    try:
        smoke(args.baseline, log, args.timeout)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        sys.exit(f"{error}. See {log}")


if __name__ == "__main__":
    main()
