#!/usr/bin/env python3
"""Drives the debug build through the places, menus, models and recorded play that fill the engine's memory
pools, and reports how full each pool got.

    python3 -I tools/memory_tour.py places              every town map and dungeon floor
    python3 -I tools/memory_tour.py menus               every menu the debug API can open, in town and in a dungeon
    python3 -I tools/memory_tour.py models              every weapon, outfit and ridepod part equipped, every monster form
    python3 -I tools/memory_tour.py recordings          replays port/tests/recordings
    python3 -I tools/memory_tour.py untested            pool set-up sites in the source no tour has reached yet
    python3 -I tools/memory_tour.py all                 places, menus, models and recordings, then untested
    python3 -I tools/memory_tour.py clips               replays every clip in build/port/clips and port/tests/recordings
                                                        and reports the ones that strayed, faulted or crashed, and
                                                        apart from those the ones whose state no longer loads
    python3 -I tools/memory_tour.py record fishing       starts the game in front set up for that minigame, says what to
                                                        play, and records your input to port/tests/recordings until you
                                                        press Enter here
    python3 -I tools/memory_tour.py record NAME town map=m03
                                                        the same from any debug API warp

It runs its own copy of build/port/darkchronicle-debug on DC2TK_DEBUG_PORT 7793 with throwaway saves, no
overrides and DC2TK_MEMORY_STRICT=1, so the first pool overflow stops the game, and with the fault watch
logging NaN or far-off camera and player values without pausing. Every visit starts from the
cap8 preset with the unlock command. Results go to build/port/memory-tour: report.json, the pools seen so far in
seen.json, and screenshots. It exits with 1 when a pool overflowed, a fault was seen or the game crashed.
"""

import json
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build" / "port" / "darkchronicle-debug"
OUT = ROOT / "build" / "port" / "memory-tour"
RECORDINGS = ROOT / "port" / "tests" / "recordings"
CLIPS = ROOT / "build" / "port" / "clips"
SOURCES = [ROOT / "build" / "port" / "copy" / "ps2" / "src", ROOT / "port" / "src"]
PORT = 7793
FLOOR_COUNTS = [6, 14, 22, 19, 21, 26, 37]
HOME_TOWN = "m01"
FIRST_FLOOR = {"dungeon": "0", "floor": "1", "event": "1010"}
TOWN_MENUS = ["main_town", "georama", "chara_change", "name", "shop", "save", "use_item", "invent", "chapter", "removal",
              "world_move", "fish_select", "item_over", "option", "costume", "chara_show", "fish_race", "spheda",
              "monster_book", "subgame_save", "spheda_score"]
DUNGEON_MENUS = ["main_dungeon", "floor_map", "chara_change_dungeon", "use_item", "invent", "chara_show_dungeon"]
EQUIPMENT = [("max_melee", 0), ("max_gun", 0), ("max_clothes", 0), ("max_hat", 0), ("max_shoes", 0),
             ("monica_melee", 1), ("monica_magic", 1), ("monica_clothes", 1), ("monica_accessory", 1),
             ("monica_boots", 1), ("robo_body", 2), ("robo_arm", 2), ("robo_leg", 2), ("robo_energy", 2)]
CHARACTERS = ["max", "monica", "ridepod"]
MINIGAMES = {
    "fishing": {
        "start": ["town map=m01", "step frames=400", "reload map=m01 x=-735 y=0 z=-157 yaw=-1.75", "step frames=300",
                  "equip chara=0 item=302 bait=317", "menu open=main_town", "step frames=200",
                  "press buttons=circle frames=4", "step frames=300"],
        "play": "Max stands at the Palm Brinks canal holding a rod with bait the canal fish like. Press cross to aim, "
                "hold the stick up for a second to cast as far as it goes (a short cast drifts back to the bank and "
                "nothing bites), then press cross to cast. When the float bobs and a red ! shows over Max, pull the "
                "stick down to hook the fish. Hold cross to reel in and let go while the gauge is near DANGER. "
                "Catch one fish.",
    },
}

game = {"process": None, "log": None, "saves": None, "overrides": None}
results = {"visits": [], "overflows": [], "faults": [], "strayed": [], "unplayable": [], "crashes": []}


def request(command, arguments=None, timeout=60):
    query = urllib.parse.urlencode(arguments or {})
    url = f"http://127.0.0.1:{PORT}/{command}" + (f"?{query}" if query else "")
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            body = response.read()
            return body if command == "screenshot" else json.loads(body)
    except urllib.error.HTTPError as error:
        return {"failed": error.read().decode(errors="replace")}
    except (OSError, ValueError):
        return None


def wait(frames):
    return request("wait", {"frames": str(frames)}, timeout=120) is not None


def alive():
    return game["process"] is not None and game["process"].poll() is None


def log_path():
    return OUT / "game.log"


def log_lines():
    try:
        return log_path().read_text(errors="replace").splitlines()
    except FileNotFoundError:
        return []


def stop():
    if alive():
        collect()
        request("quit", timeout=5)
        try:
            game["process"].wait(10)
        except subprocess.TimeoutExpired:
            game["process"].kill()
    game["process"] = None


def launch(front=False, paused=False, speed="0"):
    stop()
    if request("status", timeout=2):
        raise SystemExit(f"another game already answers on port {PORT}")
    game["saves"] = tempfile.mkdtemp(prefix="dc2tk-memory-saves-")
    game["overrides"] = game["overrides"] or tempfile.mkdtemp(prefix="dc2tk-memory-overrides-")
    flags = [] if front else ["--mute", "--background"]
    env = {**os.environ, "DC2TK_DEBUG_PORT": str(PORT), "DC2_SAVE_ROOT": game["saves"],
           "DC2TK_OVERRIDES": game["overrides"], "DC2TK_MEMORY_STRICT": "1"}
    game["log"] = open(log_path(), "a")
    game["process"] = subprocess.Popen([str(BINARY), *flags, str(ROOT / "dc2.iso")], cwd=ROOT, stdout=game["log"],
                                       stderr=game["log"], env=env)
    for _ in range(300):
        time.sleep(0.1)
        if not alive():
            raise SystemExit(f"the game exited with {game['process'].returncode} while starting; see {log_path()}")
        if request("status", timeout=1):
            break
    request("pause")
    request("speed", {"value": "0"})
    run_start(["step frames=320", "press buttons=cross frames=6", "step frames=500", "preset name=cap8",
               "step frames=600", "unlock"])
    request("speed", {"value": speed})
    if not paused:
        request("resume")


def ensure_game():
    if not alive() or request("status", timeout=5) is None:
        launch()


def collect():
    reply = request("memory", {"full": "0", "seen": "1"}, timeout=10)
    if not reply:
        return
    seen = load_seen()
    for pool in reply["pools"]:
        known = seen.setdefault(pool["offset"], {"site": pool["site"], "full": 0.0, "overflows": 0, "size": 0})
        known["full"] = max(known["full"], pool["full"])
        known["overflows"] += pool["overflows"]
        known["size"] = max(known["size"], pool["size"])
        known["used"] = known.get("used", False) or pool["peak_size"] > 0
    (OUT / "seen.json").write_text(json.dumps(seen, indent=1, sort_keys=True))
    request("memory", {"full": "2", "reset": "1"}, timeout=10)


def load_seen():
    try:
        return json.loads((OUT / "seen.json").read_text())
    except (FileNotFoundError, ValueError):
        return {}


def close_menu():
    for _ in range(16):
        status = request("status", timeout=10)
        if not status or not status.get("menu"):
            return status is not None
        request("press", {"buttons": "circle", "frames": "4"})
        wait(30)
    return False


def visit(label, steps, shot=None):
    ensure_game()
    before = len(log_lines())
    record = {"where": label}
    for command, arguments, frames in steps:
        reply = request(command, arguments, timeout=300)
        if reply and "failed" in reply:
            record.setdefault("failed", []).append(f"{command}: {reply['failed']}")
        if reply is None or (frames and not wait(frames)):
            break
    record["alive"] = alive() and request("status", timeout=10) is not None
    if record["alive"] and shot:
        picture = request("screenshot")
        if picture:
            (OUT / f"{shot}.png").write_bytes(picture)
    if record["alive"] and not close_menu():
        record["menu_left_open"] = True
        stop()
    lines = log_lines()[before:]
    over = [line for line in lines if line.startswith("memory over")]
    if over:
        record["over"] = over
        results["overflows"].append({"where": label, "lines": over})
    faults = [line for line in lines if line.startswith("fault:")]
    if faults:
        record["faults"] = faults
        results["faults"].append({"where": label, "lines": faults})
    if not record["alive"]:
        record["tail"] = lines[-12:]
        if not over:
            results["crashes"].append({"where": label, "tail": lines[-12:]})
        stop()
    results["visits"].append(record)
    print(json.dumps(record), flush=True)
    return record


def town(name, event=None):
    return ("town", {"map": name, **({"event": str(event)} if event is not None else {})}, 400)


def floor(dungeon, number):
    return ("dungeon", {"dungeon": str(dungeon), "floor": str(number), "event": "1010"}, 400)


def tour_places():
    for line in request("names", {"kind": "maps"})["names"]:
        parts = line.split(None, 2)
        if len(parts) == 3:
            visit(f"town {line}", [town(parts[0])])
    for dungeon, count in enumerate(FLOOR_COUNTS):
        for number in range(1, count + 1):
            visit(f"dungeon {dungeon} floor {number}", [floor(dungeon, number), ("defeat", {}, 200)])


def tour_menus():
    for name in TOWN_MENUS:
        visit(f"town menu {name}", [town(HOME_TOWN), ("menu", {"open": name, "param": "0"}, 300)], f"town-{name}")
    for name in DUNGEON_MENUS:
        visit(f"dungeon menu {name}", [floor(0, 1), ("menu", {"open": name, "param": "0"}, 300)], f"dungeon-{name}")


def item_ids(kind):
    ensure_game()
    reply = request("names", {"kind": "items", "type": kind}) or {"names": []}
    return [int(line.split(None, 1)[0]) for line in reply["names"]]


def tour_models():
    for kind, chara in EQUIPMENT:
        for item in item_ids(kind):
            visit(f"{kind} {item}", [town(HOME_TOWN), ("equip", {"chara": str(chara), "item": str(item)}, 10),
                                     ("character", {"who": CHARACTERS[chara]}, 400),
                                     ("menu", {"open": "chara_show", "param": str(chara)}, 300)])
    ensure_game()
    monsters = request("unlock")["monsters"]
    for monster in monsters:
        visit(f"monster {monster}", [floor(0, 1), ("character", {"who": "monica"}, 400),
                                     ("character", {"who": "monster", "monster": str(monster)}, 400)], f"monster-{monster}")


def start_steps(start):
    return [(line.split()[0], dict(word.split("=", 1) for word in line.split()[1:]), 0) for line in start]


def run_start(start):
    for command, arguments, _ in start_steps(start):
        request(command, arguments, timeout=300)


def recording_frames(recording):
    return int(recording.read_text().split()[-1])


def tour_recordings():
    for start in sorted(RECORDINGS.glob("*.json")):
        recording = start.with_suffix(".txt")
        launch(paused=True, speed="1")
        visit(f"recording {start.stem}", [*start_steps(json.loads(start.read_text())["start"]),
                                           ("replay", {"path": str(recording)}, 0), ("resume", {}, 0),
                                           ("wait", {"frames": str(recording_frames(recording) + 25)}, 0)],
              f"recording-{start.stem}")
        stop()


def clip_frames(folder):
    return len((folder / "trace.jsonl").read_text().splitlines())


def tour_clips():
    folders = sorted(folder for base in (CLIPS, RECORDINGS) if base.exists() for folder in base.iterdir()
                     if (folder / "start.state").exists() and (folder / "trace.jsonl").exists())
    for folder in folders:
        record = visit(f"clip {folder.name}", [("clip_play", {"name": str(folder)}, clip_frames(folder) + 30)])
        status = request("clip_status", timeout=10) if record["alive"] else None
        if not status:
            continue
        message = status["message"]
        kind = "unplayable" if message.startswith("savestate failed") else "strayed"
        if status["state"] != "idle" or not message.startswith("played") or status["first_apart"] >= 0:
            results[kind].append({"clip": str(folder), "state": status["state"], "message": message})
            print(json.dumps({"clip": folder.name, kind: message or status["state"]}), flush=True)


def record(name, words):
    chosen = MINIGAMES[name]["start"] if name in MINIGAMES else [" ".join(words), "step frames=400"]
    start = ["seed value=1", *chosen]
    RECORDINGS.mkdir(parents=True, exist_ok=True)
    print("setting the game up; leave the controls alone until the next message")
    launch(front=True, paused=True, speed="1")
    run_start(start)
    recording = RECORDINGS / f"{name}.txt"
    request("record", {"path": str(recording)})
    request("resume")
    if name in MINIGAMES:
        print(MINIGAMES[name]["play"])
    input(f"recording to {recording}; play, then press Enter here to stop: ")
    request("record_stop")
    (RECORDINGS / f"{name}.json").write_text(json.dumps({"start": start}) + "\n")
    stop()


def setup_sites():
    pattern = re.compile(r"\b(stSetBuffer|SetHeapMem|StartStackMode)\s*\(")
    sites = []
    for source in SOURCES:
        for path in sorted(source.rglob("*.c*")):
            for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
                if pattern.search(line) and "::" not in line.split("(")[0]:
                    sites.append(f"{path.name}:{number}")
    return sites


def seen_lines():
    offsets = list(load_seen())
    if not offsets:
        return set()
    addresses = [hex(0x100000000 + int(offset, 16) - 1) for offset in offsets]
    output = subprocess.run(["atos", "-o", str(BINARY), "-l", "0x100000000", *addresses], capture_output=True,
                            text=True).stdout
    return {match.group(1) for match in re.finditer(r"\(([^()]+:\d+)\)", output)}


def tour_untested():
    reached = seen_lines()
    reached_files = {line.split(":")[0] for line in reached}
    untested = []
    for site in setup_sites():
        name, number = site.split(":")
        near = any(line.split(":")[0] == name and abs(int(line.split(":")[1]) - int(number)) <= 2 for line in reached)
        if not near:
            untested.append(site)
    results["untested"] = untested
    print(json.dumps({"untested": len(untested), "reached_files": len(reached_files)}), flush=True)


def report():
    seen = load_seen()
    results["fullest"] = sorted(({"offset": offset, **pool} for offset, pool in seen.items() if pool.get("used")),
                                key=lambda pool: -pool["full"])[:30]
    (OUT / "report.json").write_text(json.dumps(results, indent=1))
    print(f"{len(results['visits'])} visits, {len(results['overflows'])} overflows, {len(results['faults'])} faults, "
          f"{len(results['strayed'])} clips strayed, {len(results['unplayable'])} could not load, "
          f"{len(results['crashes'])} crashes; {OUT / 'report.json'}")
    return 1 if results["overflows"] or results["faults"] or results["strayed"] or results["crashes"] else 0


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    if not BINARY.exists():
        sys.exit("build it first: python3 -I tools/port.py --movies --debug")
    OUT.mkdir(parents=True, exist_ok=True)
    if sys.argv[1] == "record":
        if len(sys.argv) < 3 or (len(sys.argv) == 3) != (sys.argv[2] in MINIGAMES):
            sys.exit(__doc__ + "\nminigames with a built-in start, named alone: " + ", ".join(MINIGAMES))
        record(sys.argv[2], sys.argv[3:])
        return
    tours = {"places": tour_places, "menus": tour_menus, "models": tour_models, "recordings": tour_recordings,
             "untested": tour_untested, "clips": tour_clips}
    names = [name for name in tours if name != "clips"] if sys.argv[1] == "all" else sys.argv[1:]
    for name in names:
        if name not in tours:
            sys.exit(__doc__)
        if name != "untested":
            ensure_game()
        tours[name]()
    stop()
    sys.exit(report())


if __name__ == "__main__":
    main()
