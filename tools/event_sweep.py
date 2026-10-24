#!/usr/bin/env python3
"""Plays every entry of the game's debug event list (event/view_pal.txt) in the debug build and reports
which ones crash or hang.

    python3 -I tools/event_sweep.py                 every row
    python3 -I tools/event_sweep.py --rows 10,26    only those rows of view_pal.txt
    python3 -I tools/event_sweep.py --from 100      rows 100 and after
    python3 -I tools/event_sweep.py --frames 1800   play each event for longer (default 900 fields)
    python3 -I tools/event_sweep.py --keep          stop at the first hang and leave that game running

It runs its own copy of build/port/darkchronicle-debug on DC2TK_DEBUG_PORT 7791 with throwaway saves, warps
the way the debug menu does (town map and event, or dungeon floor and event), presses cross now and then to
get through dialogue, and writes build/port/event-sweep.json plus the top of each crash's backtrace. It runs
with DC2TK_MEMORY_STRICT=1, so a memory pool overflow stops the game and shows up as a crash in the pool's code.
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
REPORTS = Path.home() / "Library" / "Logs" / "DiagnosticReports"
EVENTS = ROOT / "extracted" / "disc" / "event" / "view_pal.txt"
PORT = 7791


def request(command, arguments=None, timeout=60):
    query = urllib.parse.urlencode(arguments or {})
    url = f"http://127.0.0.1:{PORT}/{command}" + (f"?{query}" if query else "")
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            return json.loads(response.read())
    except (OSError, ValueError, urllib.error.HTTPError):
        return None


def launch(saves, log):
    process = subprocess.Popen([str(BINARY), "--mute", "--background", str(ROOT / "dc2.iso")], cwd=ROOT, stdout=log, stderr=log,
                               env={**os.environ, "DC2TK_DEBUG_PORT": str(PORT), "DC2_SAVE_ROOT": saves,
                                    "DC2TK_MEMORY_STRICT": "1"})
    for _ in range(300):
        time.sleep(0.1)
        if process.poll() is not None:
            raise SystemExit(f"the game exited with {process.returncode} while starting")
        if request("status", timeout=1):
            break
    request("speed", {"value": "0"})
    request("wait", {"frames": "320"})
    request("press", {"buttons": "cross", "frames": "6"})
    request("wait", {"frames": "500"})
    return process


def rows():
    lines = EVENTS.read_bytes().decode("shift_jis", "replace").splitlines()
    for number, line in enumerate(lines[1:], 2):
        columns = (line.split("\t") + [""] * 9)[:9]
        name, dungeon, floor, event, title = columns[0], columns[1], columns[2], columns[3], columns[7]
        if dungeon:
            warp = ("dungeon", {"dungeon": str(int(dungeon) - 1), "floor": floor or "0", "event": event})
        elif name:
            warp = ("town", {"map": name, "event": event})
        else:
            continue
        yield {"row": number, "title": title.strip(), "warp": warp}


def crash_report(since):
    for _ in range(40):
        reports = [path for path in REPORTS.glob("darkchronicle-debug-*.ips") if path.stat().st_mtime >= since]
        if reports:
            return max(reports, key=lambda path: path.stat().st_mtime)
        time.sleep(0.25)
    return None


def backtrace(report):
    if not report:
        return []
    body = json.loads(report.read_text().split("\n", 1)[1])
    images = body["usedImages"]
    for thread in body["threads"]:
        if thread.get("triggered"):
            return [f"{images[frame['imageIndex']].get('name', '?')} {frame.get('symbol', hex(frame.get('imageOffset', 0)))}"
                    for frame in thread["frames"][:12]]
    return []


def play(entry, frames):
    command, arguments = entry["warp"]
    reply = request(command, arguments, timeout=30)
    if not reply or not reply.get("ok"):
        return "refused"
    last = -1
    stuck = 0
    for _ in range(frames // 150):
        before = request("status", timeout=30)
        if not before:
            return "hang"
        if before["frame"] == last:
            stuck += 1
            if stuck >= 3:
                return "hang"
        else:
            stuck = 0
        last = before["frame"]
        request("wait", {"frames": "150"}, timeout=60)
        request("press", {"buttons": "cross", "frames": "4"}, timeout=30)
    return "ok"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--rows", help="comma-separated rows of view_pal.txt")
    parser.add_argument("--from", dest="start", type=int, default=0, help="first row")
    parser.add_argument("--frames", type=int, default=900, help="fields to play each event for")
    parser.add_argument("--port", type=int, default=7791, help="the debug API port of the sweep's own game")
    parser.add_argument("--keep", action="store_true", help="stop at the first hang and leave that game running")
    options = parser.parse_args()
    global PORT
    PORT = options.port
    if not BINARY.exists():
        raise SystemExit("build it first: python3 -I tools/port.py --movies --debug")
    chosen = {int(row) for row in options.rows.split(",")} if options.rows else None
    entries = [entry for entry in rows() if entry["row"] >= options.start and (chosen is None or entry["row"] in chosen)]
    saves = tempfile.mkdtemp(prefix="dc2-sweep-saves-")
    log = open(ROOT / "build" / "port" / f"event-sweep-game-{PORT}.log", "w")
    results = []
    process = launch(saves, log)
    for entry in entries:
        if process.poll() is not None:
            process = launch(saves, log)
        started = time.time()
        outcome = play(entry, options.frames)
        try:
            process.wait(timeout=3 if outcome == "hang" else 0.1)
            outcome = "crash"
        except subprocess.TimeoutExpired:
            pass
        status = request("status", timeout=5) or {}
        report = crash_report(started) if outcome == "crash" else None
        exit_status = process.poll()
        result = {**entry, "outcome": outcome, "loop": status.get("loop_name", ""), "exit": exit_status,
                  "report": str(report or ""), "backtrace": backtrace(report)}
        results.append(result)
        ending = f" (exit {exit_status})" if exit_status is not None else ""
        print(f"{entry['row']:>4} {outcome:<8} {command_text(entry):<44} {entry['title']}{ending}", flush=True)
        for line in result["backtrace"][:6]:
            print(f"         {line}", flush=True)
        if outcome == "hang" and options.keep:
            print(f"left the hung game running as process {process.pid}")
            return
        if outcome == "hang":
            process.kill()
            process.wait()
        (ROOT / "build" / "port" / "event-sweep.json").write_text(json.dumps(results, indent=2, ensure_ascii=False))
    if process.poll() is None:
        request("quit", timeout=5)
    bad = [result for result in results if result["outcome"] in ("crash", "hang")]
    print(f"{len(results) - len(bad)} of {len(results)} events played without crashing or hanging")


def command_text(entry):
    command, arguments = entry["warp"]
    return command + " " + " ".join(f"{key}={value}" for key, value in arguments.items())


if __name__ == "__main__":
    main()
