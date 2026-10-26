#!/usr/bin/env python3
"""Drives the debug build through the places, menus, models and recorded play that fill the engine's memory
pools, and reports how full each pool got.

    python3 -I tools/memory_tour.py places              every town map and dungeon floor
    python3 -I tools/memory_tour.py menus               every menu the debug API can open, in town and in a dungeon
    python3 -I tools/memory_tour.py models              every weapon, outfit and ridepod part equipped, every monster form
    python3 -I tools/memory_tour.py recordings          plays every recording in port/tests/recordings and
                                                        build/port/recordings, each in a fresh game, and reports the ones
                                                        that strayed from their trace or would not load
    python3 -I tools/memory_tour.py untested            pool set-up sites in the source no tour has reached yet
    python3 -I tools/memory_tour.py all                 places, menus, models and recordings, then untested
    python3 -I tools/memory_tour.py record fishing       starts the game in front set up for that minigame, records from
                                                        there, says what to play, and saves the recording to
                                                        port/tests/recordings when you press Enter here
    python3 -I tools/memory_tour.py record NAME town map=m03
                                                        the same from any debug API warp

It runs its own copy of build/port/dc2-debug on DC2TK_DEBUG_PORT 7793 with throwaway saves, no
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
BINARY = ROOT / "build" / "port" / "dc2-debug"
OUT = ROOT / "build" / "port" / "memory-tour"
RECORDINGS = [ROOT / "port" / "tests" / "recordings", ROOT / "build" / "port" / "recordings"]
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
                  "equip chara=0 item=302 bait=317"],
        "recorded": ["menu open=main_town", "step frames=200", "press buttons=circle frames=4", "step frames=300"],
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


def launch(front=False, paused=False, speed="0", recording=None):
    stop()
    if request("status", timeout=2):
        raise SystemExit(f"another game already answers on port {PORT}")
    game["saves"] = tempfile.mkdtemp(prefix="dc2tk-memory-saves-")
    game["overrides"] = game["overrides"] or tempfile.mkdtemp(prefix="dc2tk-memory-overrides-")
    flags = [] if front else ["--mute", "--background"]
    env = {**os.environ, "DC2TK_DEBUG_PORT": str(PORT), "DC2_SAVE_ROOT": game["saves"],
           "DC2TK_OVERRIDES": game["overrides"], "DC2TK_MEMORY_STRICT": "1"}
    if recording:
        env.update({"DC2TK_RECORDING": str(recording), "DC2TK_DEBUG_SPEED": "0"})
    game["log"] = open(log_path(), "a")
    game["process"] = subprocess.Popen([str(BINARY), *flags, str(ROOT / "dc2.iso")], cwd=ROOT, stdout=game["log"],
                                       stderr=game["log"], env=env)
    for _ in range(300):
        time.sleep(0.1)
        if not alive():
            raise SystemExit(f"the game exited with {game['process'].returncode} while starting; see {log_path()}")
        if request("status", timeout=1):
            break
    if recording:
        return
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
        known = seen.setdefault(pool["site"], {"full": 0.0, "overflows": 0, "size": 0})
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
    judge(record, log_lines()[before:])
    if not record["alive"]:
        stop()
    return record


def judge(record, lines):
    over = [line for line in lines if line.startswith("memory over")]
    if over:
        record["over"] = over
        results["overflows"].append({"where": record["where"], "lines": over})
    faults = [line for line in lines if line.startswith("fault:")]
    if faults:
        record["faults"] = faults
        results["faults"].append({"where": record["where"], "lines": faults})
    if not record["alive"]:
        record["tail"] = lines[-12:]
        if not over:
            results["crashes"].append({"where": record["where"], "tail": lines[-12:]})
    results["visits"].append(record)
    print(json.dumps(record), flush=True)


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


def recording_folders():
    return sorted(folder for base in RECORDINGS if base.exists() for folder in base.iterdir()
                  if (folder / "recording.txt").exists())


def recording_frames(folder):
    for line in (folder / "recording.txt").read_text().splitlines():
        if line.startswith("frames "):
            return int(line.split()[1])
    return 0


def play_recording(folder):
    before = len(log_lines())
    launch(recording=folder)
    status = None
    for _ in range(recording_frames(folder) // 50 + 600):
        time.sleep(0.2)
        status = request("recording_status", timeout=10) if alive() else None
        if not status or status["state"] not in ("playing", "loading playback"):
            break
    record = {"where": f"recording {folder.name}", "alive": alive() and status is not None}
    if record["alive"]:
        shot = request("screenshot", timeout=30)
        if shot:
            (OUT / f"recording-{folder.name}.png").write_bytes(shot)
    judge(record, log_lines()[before:])
    return status


def tour_recordings():
    for folder in recording_folders():
        status = play_recording(folder)
        stop()
        if not status:
            continue
        message = status["message"]
        kind = "unplayable" if not message.startswith("played") else "strayed"
        if status["state"] != "recording" or kind == "unplayable" or status["first_apart"] >= 0:
            results[kind].append({"recording": str(folder), "state": status["state"], "message": message})
            print(json.dumps({"recording": folder.name, kind: message or status["state"]}), flush=True)


def record(name, words):
    chosen = MINIGAMES[name] if name in MINIGAMES else {"start": [" ".join(words), "step frames=400"], "recorded": []}
    print("setting the game up; leave the controls alone until the next message")
    launch(front=True, paused=True, speed="1")
    run_start(chosen["start"])
    request("recording_start")
    request("resume")
    while request("recording_status", timeout=10)["state"] != "recording":
        time.sleep(0.2)
    request("pause")
    run_start(chosen["recorded"])
    request("resume")
    if name in MINIGAMES:
        print(MINIGAMES[name]["play"])
    folder = RECORDINGS[0] / name
    input(f"recording; play, then press Enter here to save it to {folder}: ")
    request("pause")
    print(json.dumps(request("recording_save", {"name": str(folder)})))
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
    return {site for site in load_seen() if site != "unknown"}


def tour_untested():
    reached = seen_lines()
    reached_files = {line.split(":")[0] for line in reached}
    untested = [site for site in setup_sites() if site not in reached]
    results["untested"] = untested
    print(json.dumps({"untested": len(untested), "reached_files": len(reached_files)}), flush=True)


def report():
    seen = load_seen()
    results["fullest"] = sorted(({"site": site, **pool} for site, pool in seen.items() if pool.get("used")),
                                key=lambda pool: -pool["full"])[:30]
    (OUT / "report.json").write_text(json.dumps(results, indent=1))
    print(f"{len(results['visits'])} visits, {len(results['overflows'])} overflows, {len(results['faults'])} faults, "
          f"{len(results['strayed'])} recordings strayed, {len(results['unplayable'])} would not play, "
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
             "untested": tour_untested}
    names = list(tours) if sys.argv[1] == "all" else sys.argv[1:]
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
