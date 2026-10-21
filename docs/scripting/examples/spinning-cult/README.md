# spinning-cult: The Spinning Cult

`spinning-cult` is a complete example quest mod that adds a custom map, a new character model, a custom item and photography invention, new character animations, and synthesized background music:

- Behind a door in Palm Brinks, a large fish rotates in a dark room above a hooded figure named Brother Gill.
- Brother Gill gives Max a Camera and asks him to photograph the fish and invent an object for the town.
- Max photographs the rotating fish from three angles, invents the **gŷros** (a gyroscope), and shows it to five townspeople in Palm Brinks.
- Each villager who sees the gŷros begins to spin in place. After three villagers turn, Palm Brinks switches to a dark lighting preset and custom music. When all five villagers turn, they gather under the fish for the ending sequence.

This directory contains no proprietary game files:

- Base maps (`m01` and `m07`) and villager models are exported from your retail ISO.
- Python scripts for Blender procedurally generate Brother Gill, the gŷros item and character models, the Fish Room map geometry, the menu icon, and the villager animations.
- A Python script synthesizes the custom music track.

## Building the mod

Run these commands from the repository root to export the base maps, generate the 3D assets and mod files in Blender, and build the modded ISO:

```bash
dc2tk map-export m01
dc2tk map-export m07
blender --background --python docs/scripting/examples/spinning-cult/make_gill.py
blender --background --python docs/scripting/examples/spinning-cult/make_gyros.py
blender --background --python docs/scripting/examples/spinning-cult/make_mod.py
blender --background --python docs/scripting/examples/spinning-cult/make_villagers.py
dc2tk build extracted/scripts dc2.iso -o spinning-cult.iso --mod spinning-cult --dev
```

### Asset generation scripts

- **`make_gill.py`**: Generates Brother Gill (`model/gill.gltf`), a 34-unit-tall robed character with an 18-bone armature and six animations: `idle`, `beckon`, `raise`, `spin`, `turn_head`, and `vanish` (a material alpha fade). `mod.json` packages this model as `mods/gill/gill.chr`.
- **`make_gyros.py`**: Generates the gyroscope (~7 units tall, compared to Max at 26 units) as three assets under `model/`:
  - `gyros_item.gltf`: The static item model for item ID `429`. `mod.json` packages it as `item/gyros.chr` (matching `model:takaraky`), and `data/items.json` sets `"model": "gyros"`.
  - `gyros.gltf`: The animated character pack (`mods/gyros/gyros.chr`) with five bones (`stand`, `outer`, `middle`, `inner`, `rotor`) and a 4-second looping `spin` animation. Map scripts load and animate it in cutscenes with `load_chara(MEMORY_STACK_EVENT, "mods/gyros/gyros.chr", "info.cfg", slot)` and `set_chara_motion(slot, "spin")`.
  - `gyros_icon.png`: The 32×32 menu icon.
- **`fish_room.py`**: Builds the Fish Room map in Blender from your export of `m07` (nighttime Palm Brinks): clears existing placements, builds a tiled room with floor, wall, and camera collision, places a 420-unit floating fish rotated 1.5 degrees per frame by a Piece animation point, adds an exit door trigger, and places the entrance trigger `door_fish` in Palm Brinks (`m01`) at `door10`.
- **`make_mod.py`**: Runs `fish_room.py` and adds:
  - A second fish model with dilated pupils for the ending sequence.
  - Three directional photography (`Invent`) bounding boxes rotating with the fish, each visible within 60 degrees of its side so photographing the fish yields **Turning** (`251`), **More Turning** (`252`), or **Endless Turning** (`253`).
  - Black fog and a red lighting preset for the ending in the Fish Room, plus a dark ninth lighting preset with red fog for Palm Brinks.
  - The synthesized music track (`compose_turned.py`).
  - The item icon patched into cell `191` (an unused reserved cell) of the disc's `texture:itemicon` sheet, quantized to the sheet's existing 256-color palette and saved to `textures/itemicon.png`.
- **`make_villagers.py`**: Exports five Palm Brinks villagers from your disc (Parn, Sheriff Blinkhorn, Giles, Aegira, and Fournet), adds two animations to each in Blender (`spin` and `stare`), and exports them into the mod.
- **`compose_turned.py`**: Synthesizes `"The Turning"` (`BG_201`), a custom music track with five WAV instruments (`musicbox`, `drone`, `choir`, `bell`, and `whisper`).
- **`data/items.json` and `data/inventions.json`**: Configure item `429` (**gŷros**, using icon cell `191` on sheet `1` and model `"gyros"`), photography ideas `251`–`253` (**Turning**, **More Turning**, and **Endless Turning**), and the invention recipe that crafts the gŷros from one Scrap of Metal.

## Script files

- **`cult.h`**: Stores the mod's persistent state inside a single 16-bit save counter (`NEW_COUNTER`), accessed through a C `union` of an `int` and a bit-field `struct` (tracking which of the five villagers have turned, the quest stage, whether town lighting has changed, and the total turn count).
- **`fishroom.c`**: The map script for the Fish Room. Controls Brother Gill, dialogue interactions, camera and item rewards, progress hints, and the ending cutscene.
- **`m01.c`**: A `#pragma target map m01` patch for Palm Brinks. Handles showing the gŷros to the five villagers, playing each villager's turning cutscene, persisting their spinning state across saves, switching the town lighting and music after the third villager turns, and facing all five villagers toward the Fish Room door after the ending.
- **`speech.h`**: Speech bubble and camera helper functions for `fishroom.c`.

## Playing the mod in-game

With `--dev`, selecting **New Game** starts in Palm Brinks (`"dev": {"start_at": "m01"}`) and advances `SAVE_SET_PROGRESS` to `3` so the townspeople are outside.

1. **Enter the Fish Room and talk to Brother Gill**: Enter the house door near the start of the street and talk to Brother Gill beneath the rotating fish to receive the Camera, Notebook, Photo Album, and a Scrap of Metal.
2. **Photograph the fish from three angles**: Equip the Camera and take three photographs as the fish rotates to capture **Turning**, **More Turning**, and **Endless Turning**.
3. **Invent and craft the gŷros**: Open the Invention menu, combine **Turning**, **More Turning**, and **Endless Turning** to invent the **gŷros**, and craft it using the Scrap of Metal. Show the gŷros to Brother Gill.
4. **Show the gŷros to five townspeople**: Return to Palm Brinks and talk to Parn, Sheriff Blinkhorn, Giles, Aegira, and Fournet while holding the gŷros.
5. **Return to the Fish Room**: Once all five villagers have turned, re-enter the Fish Room and talk to Brother Gill to watch the ending sequence.
