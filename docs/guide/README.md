# Getting Started

This guide shows you how to extract DC2 and build your first mod.

## What you need

1. **`dc2.iso`**: Your PAL PlayStation 2 `.iso` copy of DC2 (`SCES_511.90`).
2. **`dc2tk`**: Download and extract the latest release zip from [GitHub Releases](https://github.com/fakegenie/DC2-Toolkit/releases).
3. **[PCSX2](https://pcsx2.net/)**: PlayStation 2 emulator to run and test your modded `.iso`.

## Step 1: Extract the game

Copy `dc2.iso` into the `dc2tk` folder, open a terminal in that folder, and run:

```sh
dc2tk setup dc2.iso
```

(In PowerShell, run `.\dc2tk` instead of `dc2tk`. `extracted/` takes about 7 GB, and every built image is another 4.7 GB.)

This command creates the `extracted/` directory:

| Folder | Contents |
| --- | --- |
| `extracted/scripts/` | The game's 1,398 scripts decompiled into `.c` files, with their shared functions in `lib/` |
| `extracted/assets/` | Textures (`.png`), 3D models (`.gltf`), sounds (`.wav`), music (`.mid` and `.sf2`), and menu text (`.txt`), with `manifest.json` listing every asset id |
| `extracted/data/` | Game data tables as JSON (items, weapons, shops, monsters, inventions, and more), for reference |
| `extracted/maps/` | 3D maps exported as `.gltf` scenes for Blender, for reference |
| `extracted/disc/` | Unpacked raw disc files, used to rebuild the ISO |

A mod lives in its own folder, `mods/<name>/`, so `extracted/` stays a clean copy of the game. `dc2tk diff` and `dc2tk mod make` notice edits in `extracted/scripts`, `extracted/assets`, and `extracted/disc`, but not in `extracted/data` or `extracted/maps`: to change a data table or a map, copy it into a mod first (`dc2tk mod asset` or `dc2tk map-export`).

## Step 2: Make your first mod

### Example: Give Max a 999-Attack Battle Wrench

1. Copy the item table into a new mod, `mods/super-wrench/`:
   ```sh
   dc2tk mod asset dc2.iso super-wrench data:items
   ```
   This writes `mods/super-wrench/data/items.json` and a `mods/super-wrench/mod.json` that gives the mod the id `local.super-wrench` and the line `"data": {"items": "data/items.json"}`.
2. Open `mods/super-wrench/data/items.json` and replace its contents with only the record and field you change. The Battle Wrench is item `1`, and its attack is `8`:
   ```json
   {
     "items": [
       { "item": 1, "weapon": { "attack": 999 } }
     ]
   }
   ```
3. Check the mod:
   ```sh
   dc2tk mod check extracted/scripts local.super-wrench --disc dc2.iso
   ```
   It prints `1 values changed: item 1 (Battle Wrench) weapon.attack`.
4. Build `modded.iso` and launch it in PCSX2:
   ```sh
   dc2tk build extracted/scripts dc2.iso -o modded.iso --mod local.super-wrench --play
   ```

### Other ways to start a mod

Copy other assets, or a script function, into a mod the same way:

```sh
dc2tk mod asset dc2.iso super-wrench texture:e254a1
dc2tk mod override extracted/scripts super-wrench --map s51 start_castle_battle
```

Or edit scripts and assets in `extracted/` directly, list what you changed with `dc2tk diff dc2.iso`, and package it with `dc2tk mod make dc2.iso super-wrench`. See [Creating Mods](mods.md) for `mod.json` and patches.

## Finding your way around

Use these search commands to find maps, monsters, events, and save flags:

| Task | Command |
| --- | --- |
| List all maps | `dc2tk maps extracted/scripts` |
| Inspect a map's events, triggers, and spawns | `dc2tk map extracted/scripts s51` |
| Find scripts that warp into map `s51` | `dc2tk xref extracted/scripts map s51` |
| Find a monster's model, AI script, and spawns | `dc2tk xref extracted/scripts monster golem` |
| Find what triggers event `510` on map `s51` | `dc2tk xref extracted/scripts event 510 --map s51` |
| Check if save flag `54` is in use | `dc2tk flags extracted/scripts --flag 54` |

## Where to go next

- **[CLI Reference](commands.md)**: Commands and options.
- **[Creating Mods](mods.md)**: `mods/<name>/mod.json`, script patches, and `--start-at`.
- **Editing Assets**:
  - [Game Data (JSON)](data.md) — Items, weapons, shops, monsters, inventions, fishing, and Georama.
  - [Textures (PNG)](textures.md) — Replace or add 2D and 3D textures.
  - [Text and Menus](text.md) — Edit menu strings, item names, and descriptions.
  - [Sound Effects (WAV)](sounds.md) — Replace or add sound effects.
  - [Music (MIDI)](music.md) — Replace or add background music and custom instruments.
  - [3D Models (glTF)](models.md) — Edit character models, outfits, and animations.
  - [Maps and Blender](maps.md) — Edit 3D maps in Blender and add custom rooms and doors.
  - [Dungeons](dungeons.md) — Edit random dungeon floor layouts and chests.
  - [Memory Limits](memory.md) — Keep edits within PlayStation 2 memory budgets.
- **[Scripting](../scripting/README.md)**: Write custom C scripts.
