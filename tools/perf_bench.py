#!/usr/bin/env python3
"""Measures how long the debug build spends on each frame in a fixed set of scenes, so that changes to the
port can be compared.

    python3 -I tools/perf_bench.py                  every scene
    python3 -I tools/perf_bench.py --scenes genie   only the scenes whose names contain these words
    python3 -I tools/perf_bench.py --frames 500     record more frames per scene (default 250)

It runs its own copy of build/port/darkchronicle-debug on DC2TK_DEBUG_PORT 7792 with throwaway saves, warps
to each scene, lets it settle, then reads the per-frame timings. Busy time is the frame time less the wait
for the next field, and cpu is the process's CPU time per game frame across all its threads. Results go to build/port/perf-bench.json and are compared with
the previous run's.
"""

import argparse
import json
import os
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build" / "port" / "darkchronicle-debug"
RESULTS = ROOT / "build" / "port" / "perf-bench.json"
PORT = 7792
SCENES = [
    {"name": "title", "warp": None},
    {"name": "balance valley", "warp": ("town", {"map": "1"})},
    {"name": "palm brinks", "warp": ("town", {"map": "m01"})},
    {"name": "inside of train", "warp": ("town", {"map": "18"})},
    {"name": "veniccio", "warp": ("town", {"map": "g03"})},
    {"name": "underground channel", "warp": ("dungeon", {"dungeon": "0", "floor": "1"})},
    {"name": "ocean's roar cave", "warp": ("dungeon", {"dungeon": "2", "floor": "3"})},
    {"name": "dark genie", "warp": ("dungeon", {"dungeon": "6", "floor": "38", "event": "28020"})},
]
COUNTERS = ["draw_calls", "mesh_calls", "gpu_commands", "texture_uploads", "texture_decodes", "mesh_builds"]


def request(command, arguments=None, timeout=60):
    query = urllib.parse.urlencode(arguments or {})
    url = f"http://127.0.0.1:{PORT}/{command}" + (f"?{query}" if query else "")
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            return json.loads(response.read())
    except (OSError, ValueError, urllib.error.HTTPError):
        return None


def launch(saves, log):
    process = subprocess.Popen([str(BINARY), str(ROOT / "dc2.iso")], cwd=ROOT, stdout=log, stderr=log,
                               env={**os.environ, "DC2TK_DEBUG_PORT": str(PORT), "DC2_SAVE_ROOT": saves})
    for _ in range(300):
        time.sleep(0.1)
        if process.poll() is not None:
            raise SystemExit(f"the game exited with {process.returncode} while starting")
        if request("status", timeout=1):
            break
    request("window")
    request("speed", {"value": "0"})
    request("wait", {"frames": "320"})
    request("press", {"buttons": "cross", "frames": "6"})
    request("wait", {"frames": "300"})
    return process


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def cpu_seconds(process):
    text = subprocess.run(["ps", "-o", "time=", "-p", str(process.pid)], capture_output=True, text=True).stdout.strip()
    seconds = 0.0
    for part in text.replace("-", ":").split(":"):
        seconds = seconds * 60 + float(part or 0)
    return seconds


def measure(scene, frames, process):
    if scene["warp"]:
        command, arguments = scene["warp"]
        reply = request(command, arguments)
        if not reply or not reply.get("ok"):
            return None
    request("speed", {"value": "0"})
    request("wait", {"frames": "600"})
    request("speed", {"value": "1"})
    request("wait", {"frames": "10"})
    first = request("status")
    cpu_before = cpu_seconds(process)
    request("wait", {"frames": str(frames)}, timeout=frames)
    cpu_after = cpu_seconds(process)
    last = request("status")
    reply = request("stats", {"frames": str(frames)})
    if not reply:
        return None
    rows = reply["frames"]
    busy = [row["frame_ms"] - row["wait_ms"] for row in rows]
    counted = max(1, last["frame"] - first["frame"]) if first and last else len(rows)
    result = {
        "cpu_ms": round(1000.0 * (cpu_after - cpu_before) / counted, 2),
        "busy_avg": round(sum(busy) / len(busy), 2),
        "busy_p95": round(percentile(busy, 0.95), 2),
        "busy_max": round(max(busy), 2),
        "game_avg": round(sum(row["game_ms"] for row in rows) / len(rows), 2),
        "draw_avg": round(sum(row["draw_ms"] for row in rows) / len(rows), 2),
        "present_avg": round(sum(row["present_ms"] for row in rows) / len(rows), 2),
    }
    for counter in COUNTERS:
        result[counter] = round(sum(row["stats"][counter] for row in rows) / len(rows), 1)
    return result


def change(now, before):
    if not before or not before.get("busy_avg"):
        return ""
    return f"{100.0 * (now - before['busy_avg']) / before['busy_avg']:+.0f}%"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--scenes", nargs="*", help="words naming the scenes to measure")
    parser.add_argument("--frames", type=int, default=250, help="frames to record in each scene")
    options = parser.parse_args()
    if not BINARY.exists():
        raise SystemExit("build it first: python3 -I tools/port.py --movies --debug")
    scenes = [scene for scene in SCENES if not options.scenes or any(word in scene["name"] for word in options.scenes)]
    previous = json.loads(RESULTS.read_text()) if RESULTS.exists() else {}
    saves = tempfile.mkdtemp(prefix="dc2-bench-saves-")
    log = open(ROOT / "build" / "port" / "perf-bench-game.log", "w")
    process = launch(saves, log)
    results = dict(previous)
    print(f"{'scene':<22} {'cpu':>6} {'busy avg':>9} {'p95':>7} {'max':>7} {'game':>6} {'draw':>6} {'present':>8} {'draws':>6}  change")
    for scene in scenes:
        if process.poll() is not None:
            process = launch(saves, log)
        result = measure(scene, options.frames, process)
        if not result:
            print(f"{scene['name']:<22} could not be measured")
            continue
        print(f"{scene['name']:<22} {result['cpu_ms']:>6.2f} {result['busy_avg']:>9.2f} {result['busy_p95']:>7.2f} {result['busy_max']:>7.2f} "
              f"{result['game_avg']:>6.2f} {result['draw_avg']:>6.2f} {result['present_avg']:>8.2f} "
              f"{result['draw_calls']:>6.0f}  {change(result['busy_avg'], previous.get(scene['name']))}", flush=True)
        results[scene["name"]] = result
    RESULTS.write_text(json.dumps(results, indent=2) + "\n")
    if process.poll() is None:
        request("quit", timeout=5)


if __name__ == "__main__":
    main()
