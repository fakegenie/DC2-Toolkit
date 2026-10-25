#!/usr/bin/env python3
"""Golden screenshot tests for the debug build of the native port.

    python3 -I tools/golden_port.py                      run every port/tests/golden/*.json
    python3 -I tools/golden_port.py port/tests/golden/title_screen.json
    python3 -I tools/golden_port.py --update             write the screenshots as the new baselines
    python3 -I tools/golden_port.py --tolerance 4        pass pixels whose channels differ by at most 4

A scenario is a JSON list of steps, run against build/port/darkchronicle-debug through its localhost API:

    {"launch": {"replay": "port/tests/golden/boot_to_title.txt", "speed": 0, "env": {}}}
                                      first step; the game starts paused at frame 1, speed 0 is unlimited, replay
                                      plays an input recording from boot
    {"step": 300}                     run 300 frames, then pause
    {"goto": 800}                     run up to frame 800 (frames counted from boot), then pause
    {"press": "cross", "frames": 6}   hold buttons for frames; sticks with "lx", "ly", "rx", "ry"
    {"replay": "file.txt"}            play an input recording from this frame and run until it ends
    {"debugflag": 1}                  set the engine's DebugFlag
    {"speed": 1}                      change the field rate
    {"api": "command", "args": {}}    any other debug API command
    {"screenshot": "name"}            run one frame and compare its picture with the baseline called name

The game is paused between steps, so a run does not depend on how fast the machine is. Baselines are
build/port/golden/baseline/SCENARIO/NAME.png, which stays out of git because it is game imagery. A failed
comparison writes build/port/golden/actual/ and a diff image in build/port/golden/diff/ (red marks the
differing pixels). DC2TK_DEBUG_PORT picks the port (default 7783). The exit status is 1 when anything fails.
"""

import json
import os
import shutil
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PORT = int(os.environ.get("DC2TK_DEBUG_PORT", "7783"))
GOLDEN = ROOT / "build" / "port" / "golden"
SCENARIOS = ROOT / "port" / "tests" / "golden"


def request(command, arguments=None, timeout=600):
    query = urllib.parse.urlencode(arguments or {})
    url = f"http://127.0.0.1:{PORT}/{command}" + (f"?{query}" if query else "")
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            return response.read()
    except urllib.error.HTTPError as error:
        sys.exit(f"{command} failed: {error.read().decode(errors='replace')}")


def call(command, arguments=None):
    return json.loads(request(command, arguments))


def frame():
    return call("status")["frame"]


def launch(options):
    binary = ROOT / "build" / "port" / "darkchronicle-debug"
    if not binary.exists():
        sys.exit("build it first: python3 -I tools/port.py --movies --debug")
    saves = ROOT / "build" / "port" / "golden" / "saves"
    shutil.rmtree(saves, ignore_errors=True)
    saves.mkdir(parents=True)
    environment = {**os.environ, "DC2TK_DEBUG_PORT": str(PORT), "DC2TK_DEBUG_PAUSED": "1", "DC2_SAVE_ROOT": str(saves),
                   "DC2TK_DEBUG_SPEED": str(options.get("speed", 0)), **options.get("env", {})}
    if "replay" in options:
        environment["DC2TK_REPLAY_INPUT"] = str(ROOT / options["replay"])
    log = open(ROOT / "build" / "port" / "golden-run.log", "w")
    process = subprocess.Popen([str(binary), "--mute", "--background", str(ROOT / "dc2.iso")], cwd=ROOT, stdout=log, stderr=log, env=environment)
    for _ in range(300):
        time.sleep(0.1)
        if process.poll() is not None:
            sys.exit(f"the game exited with {process.returncode}; see build/port/golden-run.log")
        try:
            if frame() >= 1:
                return process
        except OSError:
            continue
    process.kill()
    sys.exit("the game did not pause at its first frame")


def step(frames):
    if frames > 0:
        request("step", {"frames": frames})


def capture():
    shot = {}
    thread = threading.Thread(target=lambda: shot.update(png=request("screenshot")))
    thread.start()
    time.sleep(0.2)
    step(1)
    thread.join()
    return shot["png"]


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
PNG_CHUNK_HEAD = struct.Struct(">I4s")
PNG_CHUNK_CRC = struct.Struct(">I")
PNG_IHDR = struct.Struct(">IIBBBBB")
PNG_BIT_DEPTH = 8
PNG_COLOR_RGB = 2
PNG_FILTER_NONE = b"\0"
RGB_BYTES = 3


def decode_png(data):
    position = len(PNG_SIGNATURE)
    pixels = b""
    while position < len(data):
        length, kind = PNG_CHUNK_HEAD.unpack_from(data, position)
        body = data[position + PNG_CHUNK_HEAD.size:position + PNG_CHUNK_HEAD.size + length]
        if kind == b"IHDR":
            width, height, *_ = PNG_IHDR.unpack(body[:PNG_IHDR.size])
        if kind == b"IDAT":
            pixels += body
        position += PNG_CHUNK_HEAD.size + length + PNG_CHUNK_CRC.size
    raw = zlib.decompress(pixels)
    stride = width * RGB_BYTES + len(PNG_FILTER_NONE)
    if any(raw[row * stride:row * stride + 1] != PNG_FILTER_NONE for row in range(height)):
        sys.exit("only the unfiltered PNGs the debug API writes are supported")
    return width, height, b"".join(raw[row * stride + len(PNG_FILTER_NONE):(row + 1) * stride] for row in range(height))


def encode_png(width, height, rgb):
    def chunk(kind, body):
        return PNG_CHUNK_HEAD.pack(len(body), kind) + body + PNG_CHUNK_CRC.pack(zlib.crc32(kind + body))

    stride = width * RGB_BYTES
    raw = b"".join(PNG_FILTER_NONE + rgb[row * stride:(row + 1) * stride] for row in range(height))
    header = PNG_IHDR.pack(width, height, PNG_BIT_DEPTH, PNG_COLOR_RGB, 0, 0, 0)
    return PNG_SIGNATURE + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def compare(actual, expected, tolerance):
    width, height, got = actual
    if (width, height) != expected[:2]:
        return None, None
    want = expected[2]
    wrong = 0
    diff = bytearray(len(got))
    for offset in range(0, len(got), 3):
        pixel = got[offset:offset + 3]
        reference = want[offset:offset + 3]
        if pixel == reference:
            shade = sum(pixel) // 6
            diff[offset:offset + 3] = bytes((shade, shade, shade))
        elif max(abs(a - b) for a, b in zip(pixel, reference)) > tolerance:
            wrong += 1
            diff[offset:offset + 3] = b"\xff\x00\x00"
        else:
            shade = sum(pixel) // 6
            diff[offset:offset + 3] = bytes((shade, shade, shade))
    return wrong, encode_png(width, height, bytes(diff))


def check_screenshot(scenario, name, png, update, tolerance):
    baseline = GOLDEN / "baseline" / scenario / f"{name}.png"
    if update:
        baseline.parent.mkdir(parents=True, exist_ok=True)
        baseline.write_bytes(png)
        print(f"  {name}: baseline written")
        return True
    if not baseline.exists():
        print(f"  {name}: FAIL no baseline; run with --update")
        return False
    wrong, diff = compare(decode_png(png), decode_png(baseline.read_bytes()), tolerance)
    if wrong == 0:
        print(f"  {name}: ok")
        return True
    actual_path = GOLDEN / "actual" / scenario / f"{name}.png"
    actual_path.parent.mkdir(parents=True, exist_ok=True)
    actual_path.write_bytes(png)
    if wrong is None:
        print(f"  {name}: FAIL the size changed; picture in {actual_path.relative_to(ROOT)}")
        return False
    diff_path = GOLDEN / "diff" / scenario / f"{name}.png"
    diff_path.parent.mkdir(parents=True, exist_ok=True)
    diff_path.write_bytes(diff)
    print(f"  {name}: FAIL {wrong} pixels differ; diff in {diff_path.relative_to(ROOT)}")
    return False


def run_step(entry, scenario, update, tolerance):
    if "step" in entry:
        step(entry["step"])
    elif "goto" in entry:
        step(entry["goto"] - frame())
    elif "press" in entry:
        arguments = {key: value for key, value in entry.items() if key != "press"}
        request("press", {"buttons": entry["press"], **arguments})
    elif "replay" in entry:
        started = call("replay", {"path": str(ROOT / entry["replay"])})
        step(started["frames"])
    elif "debugflag" in entry:
        request("debugflag", {"value": entry["debugflag"]})
    elif "speed" in entry:
        request("speed", {"value": entry["speed"]})
    elif "api" in entry:
        request(entry["api"], entry.get("args", {}))
    elif "screenshot" in entry:
        return check_screenshot(scenario, entry["screenshot"], capture(), update, tolerance)
    else:
        sys.exit(f"{scenario}: unknown step {entry}")
    return True


def run_scenario(path, update, tolerance):
    scenario = path.stem
    steps = json.loads(path.read_text())
    print(scenario)
    options = steps[0]["launch"] if steps and "launch" in steps[0] else {}
    process = launch(options)
    passed = True
    try:
        for entry in steps[1:] if "launch" in steps[0] else steps:
            passed = run_step(entry, scenario, update, tolerance) and passed
    finally:
        try:
            request("quit", timeout=5)
        except OSError:
            pass
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
    return passed


def main():
    words = sys.argv[1:]
    update = "--update" in words
    tolerance = 0
    if "--tolerance" in words:
        at = words.index("--tolerance")
        tolerance = int(words[at + 1])
        del words[at:at + 2]
    paths = [Path(word) for word in words if word != "--update"] or sorted(SCENARIOS.glob("*.json"))
    results = [run_scenario(path, update, tolerance) for path in paths]
    print(f"{sum(results)} of {len(results)} scenarios passed")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
