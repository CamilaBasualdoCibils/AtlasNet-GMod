#!/usr/bin/env python3
"""Check the actual AtlasNet-TUI against the running GMod Control Panel."""
import argparse
import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import struct
import subprocess
import termios
import time

ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


def smoke(program, url, expected, expected_x, capture):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 28, 110, 0, 0))
    process = subprocess.Popen(
        [program, "--connect", url], stdin=slave, stdout=slave, stderr=slave,
        env=dict(os.environ, TERM="xterm-256color"),
    )
    os.close(slave)
    terminal = bytearray()
    details = {}
    try:
        end = time.monotonic() + 20
        next_hover = 0
        target = 0
        success = False
        while time.monotonic() < end and process.poll() is None:
            if not select.select([master], [], [], 0.1)[0]:
                continue
            try:
                data = os.read(master, 65536)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                break
            terminal.extend(data)
            if b"\x1b[6n" in data:
                os.write(master, b"\x1b[1;1R")
            # Keep enough FTXUI frames to recover the currently rendered map.
            plain = ANSI.sub("", terminal[-100000:].decode(errors="replace"))
            end_map = plain.rfind("╰")
            start_map = plain.rfind("╭", 0, end_map)
            if end_map < 0 or start_map < 0:
                continue
            frame = plain[start_map:end_map]
            if "Connected to" not in frame:
                continue
            client = re.search(r"Entity ID: ([\w-]+) \| Client ID: ([\w-]+)", frame)
            shard = re.search(r"Current shard ID: ([\w-]+) \| Position: \(([^,]+),\s*([^,]+),\s*([^)]*)\)", frame)
            region = re.search(r"Region ID: (-?\d+) \| Shard ID: ([\w-]+)", frame)
            if client and shard and region:
                details[client[2]] = (client[1], shard[1], region[1], tuple(map(float, shard.group(2, 3, 4))))
            if all(client_id in details and details[client_id][1] == shard_id
                   for client_id, shard_id in expected.items()) and all(
                       client_id in details and details[client_id][3][0] * sign > 0
                       for client_id, sign in expected_x.items()):
                success = bool(details)
                if success:
                    break
            if time.monotonic() < next_hover:
                continue
            # PTY output can contain CRCRLF. Split only on LF to keep mouse
            # coordinates aligned with the rendered terminal rows.
            markers = [(column, row) for row, line in enumerate(frame.split("\n"), 1)
                       for column, character in enumerate(line, 1) if character == "△"]
            if markers:
                column, row = markers[target % len(markers)]
                os.write(master, f"\x1b[<35;{column};{row}M".encode())
                target += 1
                next_hover = time.monotonic() + 0.12
        if capture:
            capture.write_bytes(terminal)
        if not success:
            raise RuntimeError(f"TUI did not render the expected client and shard details. Observed {details}")
        os.write(master, b"q")
        if process.wait(timeout=4) != 0:
            raise RuntimeError("AtlasNet-TUI exited unsuccessfully")
        for client, (entity, shard, region, (x, y, z)) in sorted(details.items()):
            print(f"TUI client={client} entity={entity} shard={shard} region={region} position=({x:g}, {y:g}, {z:g})")
    finally:
        os.close(master)
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("program", type=Path)
    parser.add_argument("--url", default="ws://127.0.0.1:27270")
    parser.add_argument("--expect", action="append", default=[], metavar="CLIENT=SHARD")
    parser.add_argument("--expect-x-sign", action="append", default=[], metavar="CLIENT=-1|1")
    parser.add_argument("--capture", type=Path)
    args = parser.parse_args()
    expected_x = {}
    for entry in args.expect_x_sign:
        client, sign = entry.split("=", 1)
        if sign not in ("-1", "1"):
            parser.error("expected X sign must be -1 or 1")
        expected_x[client] = int(sign)
    smoke(args.program.resolve(), args.url, dict(item.split("=", 1) for item in args.expect), expected_x, args.capture)
