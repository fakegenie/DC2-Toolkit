"""Makes the spinning-cult mod's maps, music and icon: the Fish Room (fish_room.py) with what
the cult needs, Palm Brinks with the door and a dark light set, the cult's tune, and the gyros'
icon in the menus' icon sheet, written into mods/spinning-cult with this folder's mod.json, scripts,
data and models (Brother Gill's and the gyros'):

    dc2tk map-export m01
    dc2tk map-export m07
    blender --background --python docs/coding/examples/spinning-cult/make_gill.py
    blender --background --python docs/coding/examples/spinning-cult/make_gyros.py
    blender --background --python docs/coding/examples/spinning-cult/make_mod.py [-- DC2TK ISO]
    blender --background --python docs/coding/examples/spinning-cult/make_villagers.py

DC2TK and ISO default to build/release/dc2tk.exe and pal.iso in the repository.

The Fish Room is built by fish_room.py (the room, the fish turning, the door), and then, as one would in Blender with the dc2tk_maps add-on:
- three Invent boxes round the fish, turning with it, each seen from a third of the way round:
  a photo gives "Turning" (251), "More Turning" (252) or "Endless Turning" (253), by where the
  fish is in its turn;
- a second light set, red, for the ending.

Palm Brinks gets the door point door_fish where the closed house door door10 was and a ninth light set, dark with a dried-blood fog, that the town goes to once three
have turned.

The icon: the menus draw an item's icon from the icon sheets in img/allitem.img and the menu packs'
copies of it (texture:itemicon, 8 by 24 cells of 32x32 sharing one 256-colour palette), cell `icon`
of sheet `icon_sheet`. Cell 191 of the item sheet is the game's own placeholder, a "reserved" mark
no item points at. This script exports the sheet from your disc (dc2tk mod asset), draws
model/gyros_icon.png (made by make_gyros.py) into that cell in the sheet's own colours (each pixel
the nearest of the palette's opaque colours, the clear ones its clear colour) and writes it as
textures/itemicon.png with the same palette, so build changes those 1,024 pixels and nothing else;
data/items.json points the gyros (item 429) at it.

Nothing of the game's is in this folder: the scenes and the icon sheet come from your own disc.
"""

import copy
import importlib.util
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(HERE))))
MAPS = os.path.join(REPO, "extracted", "maps")
MOD = os.path.join(REPO, "mods", "spinning-cult")
OWN_FILES = ("mod.json", "cult.h", "speech.h", "m01.c", "fishroom.c")
DATA_FILES = ("items.json", "inventions.json")

IDEAS = (251, 252, 253)  # Turning, More Turning, Endless Turning
TOWN_DARK_LIGHT = 8
GYROS = 429  # the item
ICON_SHEET, ICON_CELL = 1, 191  # itemicon's cell 191: the game's "reserved" mark, no item's icon


def arguments():
    given = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    dc2tk = given[0] if len(given) > 0 else os.path.join(REPO, "build", "release", "dc2tk.exe")
    iso = given[1] if len(given) > 1 else os.path.join(REPO, "pal.iso")
    return os.path.abspath(dc2tk), os.path.abspath(iso)


def png_chunks(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    at, chunks = 8, []
    while at < len(data):
        size, kind = struct.unpack(">I4s", data[at:at + 8])
        chunks.append((kind, data[at + 8:at + 8 + size]))
        at += 12 + size
    return chunks


def png_rows(chunks):
    """The PNG's rows, unfiltered, as bytearrays; its width, height and bytes a pixel (8-bit only)."""
    width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", dict(chunks)[b"IHDR"])
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    if depth != 8 or interlace:
        raise ValueError("only 8-bit PNGs without interlacing")
    raw = zlib.decompress(b"".join(body for kind, body in chunks if kind == b"IDAT"))
    bpp, stride, rows, prev = channels, width * channels, [], bytearray(width * channels)
    for y in range(height):
        start = y * (stride + 1)
        kind, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if kind == 1:
                line[i] = (line[i] + a) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + b) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        rows.append(line)
        prev = line
    return rows, width, height, channels


def place_icon(dc2tk, iso, target):
    """The disc's item icon sheet with the gyros' icon in cell ICON_CELL, written to target."""
    with open(os.path.join(HERE, "model", "gyros_icon.png"), "rb") as file:
        icon_rows, w, h, channels = png_rows(png_chunks(file.read()))
    if (w, h, channels) != (32, 32, 4):
        raise SystemExit("model/gyros_icon.png should be 32x32 RGBA: make it with make_gyros.py")
    with tempfile.TemporaryDirectory() as folder:
        subprocess.run([dc2tk, "mod", "asset", iso, "sheet", "texture:itemicon", "data:items", "--mods", folder], check=True)
        with open(os.path.join(folder, "sheet", "textures", "itemicon.png"), "rb") as file:
            sheet = file.read()
        with open(os.path.join(folder, "sheet", "data", "items.json"), encoding="utf-8") as file:
            items = json.load(file)["items"]
    taken = [i["item"] for i in items if (i["icon_sheet"], i["icon"]) == (ICON_SHEET, ICON_CELL) and i["item"] != GYROS]
    if taken:
        raise SystemExit("the disc's items %s already use icon %d of sheet %d" % (taken, ICON_CELL, ICON_SHEET))
    chunks = png_chunks(sheet)
    rows, width, height, channels = png_rows(chunks)
    found = dict(chunks)
    if channels != 1 or b"PLTE" not in found or (width, height) != (256, 768):
        raise SystemExit("the icon sheet is not the 256x768 paletted picture expected")
    plte, trns = found[b"PLTE"], found.get(b"tRNS", b"")
    palette = [tuple(plte[3 * i:3 * i + 3]) for i in range(len(plte) // 3)]
    alphas = list(trns) + [255] * (len(palette) - len(trns))
    opaque = [i for i, a in enumerate(alphas) if a == 255]
    used = [0] * len(palette)
    for row in rows:
        for i in row:
            used[i] += 1
    clear = max((i for i, a in enumerate(alphas) if a == 0), key=lambda i: used[i])
    cache = {}

    def nearest(rgb):
        # weighted by how the eye sees each primary
        if rgb not in cache:
            r, g, b = rgb
            cache[rgb] = min(opaque, key=lambda i: 0.3 * (palette[i][0] - r) ** 2 + 0.59 * (palette[i][1] - g) ** 2
                             + 0.11 * (palette[i][2] - b) ** 2)
        return cache[rgb]

    x0, y0 = (ICON_CELL % 8) * 32, (ICON_CELL // 8) * 32
    for y in range(32):
        for x in range(32):
            r, g, b, a = icon_rows[y][4 * x:4 * x + 4]
            rows[y0 + y][x0 + x] = nearest((r, g, b)) if a >= 128 else clear
    data = zlib.compress(b"".join(b"\x00" + bytes(row) for row in rows), 9)
    out = [b"\x89PNG\r\n\x1a\n"]
    for kind, body in chunks:
        if kind == b"IDAT":
            if data is None:
                continue
            body, data = data, None
        out.append(struct.pack(">I4s", len(body), kind) + body + struct.pack(">I", zlib.crc32(kind + body)))
    os.makedirs(os.path.dirname(target), exist_ok=True)
    with open(target, "wb") as file:
        file.write(b"".join(out))


def load_fish_room():
    """fish_room.py, which builds the Fish Room, as a module."""
    path = os.path.join(HERE, "fish_room.py")
    spec = importlib.util.spec_from_file_location("fish_room", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def extend_room(fish_room):
    """What the cult adds to the Fish Room, after fish_room.py has built it."""

    def then(root, _fish_placement):
        from dc2tk_maps import c2data

        centre = fish_room.game(0, fish_room.FISH_HEIGHT, 0)
        # the photos: three boxes round the fish, one idea each, every one seen only from within 60
        # degrees of its own side (an invent point's angle) and turned a third of a turn from the
        # next, so from wherever the camera is exactly one faces it. The fish's turning point turns
        # the whole placement (its piece "") rather than its piece, so the boxes, which a
        # placement's points hang from, turn with the fish: which idea a photo gives depends on
        # where the fish is in its turn. The camera looks only at the invent points of placements
        # that are drawn (CMap::InScreenFunc walks the placements), so the boxes go in the fish's
        # placement, not the map's own points.
        spin = next(child for child in _fish_placement.children if c2data.role(child) == "point")
        data = c2data.get(spin)
        data["data"]["piece"] = ""
        data["data"].pop("frame", None)
        c2data.put(spin, data)
        piece = next(child for child in _fish_placement.children if c2data.role(child) == "piece")
        for number, idea_id in enumerate(IDEAS):
            fish_room.select_only([piece])
            idea = fish_room.add_point("INVENT", "fish_idea_%d" % (number + 1), centre, in_placement=True)
            idea.rotation_euler = (0.0, 0.0, math.radians(120.0 * number))
            data = c2data.get(idea)
            data["data"]["id"] = idea_id
            data["data"]["min"] = [-260.0, -140.0, -140.0]
            data["data"]["max"] = [260.0, 140.0, 140.0]
            data["data"]["unknown_float"] = 1600.0
            data["data"]["angle"] = 60.0
            c2data.put(idea, data)
        # light sets: the dark room fades into black fog; set 1 is the ending's red
        data = c2data.get(root)
        sets = data["map"]["settings"]["light_sets"]
        sets[0]["fog_enable"] = 1
        sets[0]["fog"] = dict(sets[0].get("fog", {}), near=700, far=1800, color=[0, 0, 0])
        red = copy.deepcopy(sets[0])
        red["index"] = 1
        red["bgcolor"] = [24, 0, 0]
        red["bgcolor2"] = [0, 0, 0]
        red["ambient"] = [140, 40, 40]
        red["fog"] = dict(red["fog"], near=150, far=1300, color=[90, 0, 0])
        sets.append(red)
        c2data.put(root, data)

    return then


def darken_town(scene_path):
    """Adds light set 8 to Palm Brinks: night's set, darker, red, with a close fog."""
    with open(scene_path, encoding="utf-8") as file:
        scene = json.load(file)
    root = next(node for node in scene["nodes"] if (node.get("extras", {}).get("c2") or {}).get("role") == "map")
    sets = root["extras"]["c2"]["map"]["settings"]["light_sets"]
    if any(light_set.get("index") == TOWN_DARK_LIGHT for light_set in sets):
        return
    night = next((light_set for light_set in sets if light_set.get("index") == 5), sets[-1])
    dark = copy.deepcopy(night)
    dark["index"] = TOWN_DARK_LIGHT
    dark["bgcolor"] = [10, 0, 0]
    dark["bgcolor2"] = [0, 0, 0]
    dark["ambient"] = [60, 25, 25]
    for light in dark.get("lights", []):
        light["color"] = [min(255, int(c * f)) for c, f in zip(light.get("color", [80, 80, 80]), (0.9, 0.35, 0.35))]
    dark["fog_enable"] = 1
    dark["fog"] = dict(dark.get("fog", {}), near=120, far=900, color=[45, 4, 4])
    sets.append(dark)
    with open(scene_path, "w", encoding="utf-8") as file:
        json.dump(scene, file, ensure_ascii=False)


def compose(folder):
    path = os.path.join(HERE, "compose_turned.py")
    spec = importlib.util.spec_from_file_location("spinning_cult_compose", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.write(folder)


def main():
    for name in ("m01", "m07"):
        if not os.path.isfile(os.path.join(MAPS, name, name + ".gltf")):
            raise SystemExit("export the map first: dc2tk map-export %s" % name)
    gill = os.path.join(HERE, "model", "gill.gltf")
    if not os.path.isfile(gill):
        raise SystemExit("make Brother Gill first: blender --background --python %s" % os.path.join(HERE, "make_gill.py"))
    for name in ("gyros.gltf", "gyros_item.gltf", "gyros_icon.png"):
        if not os.path.isfile(os.path.join(HERE, "model", name)):
            raise SystemExit("make the gyros first: blender --background --python %s" % os.path.join(HERE, "make_gyros.py"))
    dc2tk, iso = arguments()
    sys.path.insert(0, os.path.join(REPO, "blender-plugin"))
    import addon_utils
    import bpy

    fish_room = load_fish_room()
    for folder in ("maps", "music", "model", "data", "textures"):
        if os.path.isdir(os.path.join(MOD, folder)):
            shutil.rmtree(os.path.join(MOD, folder))
    os.makedirs(os.path.join(MOD, "data"), exist_ok=True)
    for name in OWN_FILES:
        target = os.path.join(MOD, name)
        if name == "mod.json" and os.path.isfile(target):
            # keep the lines make_villagers.py added
            with open(target, encoding="utf-8") as file:
                kept = json.load(file)
            with open(os.path.join(HERE, name), encoding="utf-8") as file:
                made = json.load(file)
            for key, value in kept.get("files", {}).items():
                made.setdefault("files", {}).setdefault(key, value)
            with open(target, "w", encoding="utf-8") as file:
                json.dump(made, file, ensure_ascii=False, indent=1)
                file.write("\n")
            continue
        shutil.copyfile(os.path.join(HERE, name), target)
    for name in DATA_FILES:
        shutil.copyfile(os.path.join(HERE, "data", name), os.path.join(MOD, "data", name))
    shutil.copytree(os.path.join(HERE, "model"), os.path.join(MOD, "model"))
    place_icon(dc2tk, iso, os.path.join(MOD, "textures", "itemicon.png"))
    shutil.copytree(os.path.join(MAPS, "m01"), os.path.join(MOD, "maps", "m01"))
    fish_room.add_town_door(os.path.join(MOD, "maps", "m01", "m01.gltf"))
    darken_town(os.path.join(MOD, "maps", "m01", "m01.gltf"))
    compose(os.path.join(MOD, "music"))

    bpy.ops.wm.read_factory_settings(use_empty=True)
    addon_utils.enable("dc2tk_maps", default_set=True, persistent=False)
    fish_room.build_room(mod=MOD, then=extend_room(fish_room))
    print("spinning-cult written to %s" % MOD)


if __name__ == "__main__":
    main()
