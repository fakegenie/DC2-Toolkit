#!/usr/bin/env python3
"""Talks to a running build/port/darkchronicle-debug through its localhost API.

    python3 -I tools/debug.py help                       list the game's commands
    python3 -I tools/debug.py status
    python3 -I tools/debug.py press buttons=cross frames=6
    python3 -I tools/debug.py step frames=10
    python3 -I tools/debug.py screenshot out.png         or windowshot out.png for the debug window
    python3 -I tools/debug.py screenshot out.png full=1  the picture at the internal resolution (--scale)
    python3 -I tools/debug.py stats frames=50 --summary
    python3 -I tools/debug.py launch [--debug]           start the debug build and wait for its API
    python3 -I tools/debug.py launch --scale 2 --msaa 4  render at twice the PS2 resolution with 4x MSAA

DC2TK_DEBUG_PORT picks the port (default 7777).
"""

import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PORT = int(os.environ.get("DC2TK_DEBUG_PORT", "7777"))
IMAGES = {"screenshot", "windowshot", "texture", "grab"}


def request(command, arguments, timeout=120):
    query = urllib.parse.urlencode(arguments)
    url = f"http://127.0.0.1:{PORT}/{command}" + (f"?{query}" if query else "")
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.read()


def launch(extra):
    binary = ROOT / "build" / "port" / "darkchronicle-debug"
    if not binary.exists():
        sys.exit("build it first: python3 -I tools/port.py --movies --debug")
    log = open(ROOT / "build" / "port" / "debug-run.log", "w")
    process = subprocess.Popen([str(binary), *extra, str(ROOT / "dc2.iso")], cwd=ROOT, stdout=log, stderr=log,
                               env={**os.environ, "DC2TK_DEBUG_PORT": str(PORT)})
    for _ in range(300):
        time.sleep(0.1)
        if process.poll() is not None:
            sys.exit(f"the game exited with {process.returncode}; see build/port/debug-run.log")
        try:
            status, body = request("status", {}, timeout=1)
            print(body.decode())
            return
        except OSError:
            continue
    sys.exit("the game did not answer on its API")


def summary(frames):
    keys = ["frame_ms", "game_ms", "draw_ms", "present_ms", "wait_ms"]
    counters = ["draw_calls", "mesh_calls", "gpu_commands", "texture_uploads", "texture_decodes", "allocations",
                "disc_bytes", "sorted_draws"]
    for key in keys:
        values = [frame[key] for frame in frames]
        print(f"{key:>16}: avg {sum(values) / len(values):8.2f}  max {max(values):8.2f}")
    for key in counters:
        values = [frame["stats"][key] for frame in frames]
        print(f"{key:>16}: avg {sum(values) / len(values):8.1f}  max {max(values):8d}")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    command = sys.argv[1]
    if command == "launch":
        launch(sys.argv[2:])
        return
    words = sys.argv[2:]
    paths = [word for word in words if "=" not in word and word != "--summary"]
    output = paths[0] if command in IMAGES and paths else None
    words = [word for word in words if word != output]
    show_summary = "--summary" in words
    words = [word for word in words if word != "--summary"]
    arguments = dict(word.split("=", 1) for word in words)
    if command == "screenshot" and arguments.pop("full", "0") != "0":
        command = "grab"
    status, body = request(command, arguments)
    if command in IMAGES and status == 200:
        path = Path(output or f"build/port/{command}.png")
        path.write_bytes(body)
        print(path)
        return
    if show_summary and status == 200:
        summary(json.loads(body)["frames"])
        return
    text = body.decode(errors="replace")
    try:
        print(json.dumps(json.loads(text), indent=2))
    except ValueError:
        print(text)
    if status != 200:
        sys.exit(1)


if __name__ == "__main__":
    main()
