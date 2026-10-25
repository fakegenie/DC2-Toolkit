#!/usr/bin/env python3
"""Plays recordings in the debug build and reports how fast it draws them.

    python3 -I tools/bench.py                        every recording in port/tests/recordings, paced
    python3 -I tools/bench.py swamp --unpaced        the game ticks as fast as it can
    python3 -I tools/bench.py swamp -- --msaa 4      flags after -- go to the game
    python3 -I tools/bench.py --front                in a window in front, which is how a player sees it

Each recording is copied to build/port/recordings/bench-NAME and played from there, so the game writes
play.jsonl next to it. Shown frames are ticks plus the frames drawn between them. gpu_ms is the time the
GPU was busy, measured from each submission to its fence.
"""

import json
import os
import shutil
import statistics
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RECORDINGS = ROOT / "port" / "tests" / "recordings"
PORT = int(os.environ.get("DC2TK_DEBUG_PORT", "7790"))
KEYS = ["frame_ms", "game_ms", "present_ms", "wait_ms", "gpu_ms"]


def request(command, arguments, timeout=300):
    query = urllib.parse.urlencode(arguments)
    url = f"http://127.0.0.1:{PORT}/{command}" + (f"?{query}" if query else "")
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            return json.loads(response.read())
    except urllib.error.HTTPError as error:
        return json.loads(error.read())


def launch(front, flags):
    binary = ROOT / "build" / "port" / "darkchronicle-debug"
    if not binary.exists():
        sys.exit("build it first: python3 -I tools/port.py --movies --debug")
    log = open(ROOT / "build" / "port" / "bench.log", "w")
    shown = [] if front else ["--background"]
    process = subprocess.Popen([str(binary), "--mute", *shown, *flags, str(ROOT / "dc2.iso")], cwd=ROOT, stdout=log,
                               stderr=log, env={**os.environ, "DC2TK_DEBUG_PORT": str(PORT)})
    for _ in range(300):
        time.sleep(0.1)
        if process.poll() is not None:
            sys.exit(f"the game exited with {process.returncode}; see build/port/bench.log")
        try:
            request("status", {}, timeout=1)
            return process
        except OSError:
            continue
    sys.exit("the game did not answer on its API")


def stop(process):
    try:
        request("quit", {}, timeout=5)
    except OSError:
        pass
    try:
        process.wait(20)
    except subprocess.TimeoutExpired:
        process.kill()


def copy(name):
    folder = ROOT / "build" / "port" / "recordings" / f"bench-{name}"
    shutil.rmtree(folder, ignore_errors=True)
    shutil.copytree(RECORDINGS / name, folder)
    return folder


def play(name, unpaced):
    folder = copy(name)
    request("speed", {"value": "0" if unpaced else "1"})
    request("toggle", {"name": "unpaced", "value": "1" if unpaced else "0"})
    request("resume", {})
    started = request("recording_play", {"name": str(folder.relative_to(ROOT))})
    if "error" in started:
        sys.exit(f"{name}: {started['error']}")
    for _ in range(1200):
        time.sleep(0.5)
        if request("recording_status", {})["state"] not in ("playing", "loading playback"):
            break
    passes = [frame["stats"]["render_passes"] for frame in request("stats", {"frames": "600"})["frames"]]
    rows = [json.loads(line) for line in open(folder / "play.jsonl")][5:]
    return rows, passes


def report(name, rows, passes):
    seconds = sum(row["frame_ms"] for row in rows) / 1000
    shown = len(rows) + sum(row["replays"] for row in rows)
    print(f"{name}: {len(rows)} ticks, {len(rows) / seconds:.1f} ticks/s, {shown / seconds:.1f} drawn/s, "
          f"{statistics.mean(passes):.1f} render passes a frame")
    for key in KEYS:
        values = sorted(row[key] for row in rows)
        print(f"  {key:>10}: mean {statistics.mean(values):7.2f}  median {values[len(values) // 2]:7.2f}  "
              f"p95 {values[int(len(values) * 0.95)]:7.2f}  max {values[-1]:7.2f}")


def main():
    words = sys.argv[1:]
    flags = words[words.index("--") + 1:] if "--" in words else []
    words = words[:words.index("--")] if "--" in words else words
    names = [word for word in words if not word.startswith("--")] or sorted(path.name for path in RECORDINGS.iterdir())
    process = launch("--front" in words, flags)
    try:
        for name in names:
            rows, passes = play(name, "--unpaced" in words)
            report(name, rows, passes)
    finally:
        stop(process)


if __name__ == "__main__":
    main()
