# Creating Mods

A mod is a folder inside `mods/` (for example, `mods/castle/`). Storing changes in `mods/` keeps `extracted/` unmodified so you can combine multiple mods at build time:

```sh
dc2tk build extracted/scripts dc2.iso -o modded.iso --mod castle --mod another-mod
```

See two complete example mods in `docs/scripting/examples/` (copy one into `mods/` to build it):

- **[calculator](../scripting/examples/calculator/calculator.c)**: In-game calculator with a custom 3D model, 2D UI sprites, and sound effects.
- **[spinning-cult](../scripting/examples/spinning-cult/README.md)**: Quest mod with a custom Blender room, a door in Palm Brinks, a custom NPC, a custom item, and new music.

## Mod folder layout and `mod.json`

A mod folder contains `mod.json` alongside any `.c` script patches and asset files:

```text
mods/my-mod/
├── mod.json            # Declares replaced and added assets, data tables, and settings
├── s51.c               # Script patch (optional)
├── data/
│   └── items.json      # Edited item stats (optional)
└── art/
    └── soldier.png     # Replacement texture (optional)
```

Example `mod.json`:

```json
{
  "title": "My Mod",
  "version": "1.0.0",
  "requires": ["other-mod"],
  "start_at": "s51:story_50",
  "replace": {
    "data:items": "data/items.json",
    "data:monsters": "data/monsters.json",
    "texture:e254a1": "art/soldier.png",
    "sound:SE_CURSOR_MOVE": "sounds/click.wav",
    "music:BG_000_000": "music/title.mid",
    "text:en/shopname": "text/en/shopname.txt",
    "model:e33a": "models/e33a",
    "map:s17": "maps/s17/s17.gltf"
  },
  "add": {
    "sound:SE_MY_BEEP": {"bank": "SP_042", "file": "sounds/beep.wav"},
    "music:BG_201": {"file": "music/theme.mid", "like": "BG_193"},
    "map:fishroom": {"file": "maps/fishroom/fishroom.gltf", "like": "m07", "title": "Fish Room", "script": "fishroom.c"}
  }
}
```

| Key | Meaning |
| --- | --- |
| `"title"`, `"version"` | Shown by `dc2tk mod list`. |
| `"requires"`, `"after"` | Mods this one builds on (see [Mods that build on mods](#mods-that-build-on-mods)). |
| `"start_at"` | Where **New Game** starts in a test build (see [Trying a change quickly](#trying-a-change-quickly)). |
| `"replace"` | Game assets and data tables the mod changes, by id. A line is a file path, or an object with `"file"` and options. |
| `"add"` | New assets: sound effects, music files, maps, and game files. |

Options a line can take, besides `"file"`:

| Option | Lines | Meaning |
| --- | --- | --- |
| `"only"` | `texture:`, `model:`, `sound:` | Replace the asset only under the disc paths listed (for example, `["map/a"]`) |
| `"volume"` | `sound:` | The sound's volume |
| `"bank"` | `sound:` under `"add"` | The bank to add the sound to |
| `"instruments"` | `music:` | WAV files as new instruments (see [Music](music.md)) |
| `"like"` | `music:`, `map:`, `file:` under `"add"` | The existing asset to copy as a template |
| `"title"`, `"script"`, `"compact"` | `map:` under `"add"` | The new map's title, its C script, and whether deleted placements are dropped (see [Maps](maps.md)) |
| `"programs"` | `map:` | Script programs for the map's event points, by point name (see [Maps](maps.md)) |

## Game assets by id

Reference assets in `mod.json` by their **asset ID** (listed in `extracted/assets/manifest.json`). `dc2tk build` updates every copy of that asset across the disc.

Use `dc2tk mod asset` to copy an asset or data table into your mod and add it to `mod.json`:

```sh
dc2tk mod asset dc2.iso my-mod data:items
dc2tk mod asset dc2.iso my-mod texture:e254a1
dc2tk mod asset dc2.iso my-mod model:e33a
dc2tk mod asset dc2.iso my-mod sound:SE_CURSOR_MOVE
dc2tk mod asset dc2.iso my-mod music:BG_000_000
dc2tk mod asset dc2.iso my-mod text:en/shopname
dc2tk mod asset dc2.iso my-mod bank:SP_003
```

A pattern such as `'model:c02fuku*'` copies every asset it matches (quote it in your shell). Pass `--as png`, `--as wav`, `--as gltf`, `--as txt`, or `--as mid` with a disc file path (such as `chara/c02a.chr`) to export a whole game file instead.

## Patches

Place `.c` patch files in your mod folder to replace or add functions in game scripts. Because patches contain only changed functions, multiple mods can patch the same script.

Run `dc2tk mod override` to generate a patch for an existing function:

```sh
dc2tk mod override extracted/scripts castle --map s51 start_castle_battle
```

This command creates `mods/castle/s51.c`:

It holds a copy of the function. Edit it, and call `original()` to run the game's own version:

```c
// Every script of map s51: its five language variants
#pragma target map s51
#include <event.h>
#include <disc.h>

// A save flag of the mod's own
NEW_FLAG(FIGHT_STARTED);

// Replaces the script's start_castle_battle
#pragma override
void start_castle_battle(void)
{
    original();
    set_flag(FIGHT_STARTED, 1);
}
```

| Directive / Macro | Purpose |
| --- | --- |
| `#pragma target <kind> <name>` | Select scripts to patch (`map s51`, `monster golem`, `script mainchr/c01.c`, or `library lib/...`). `map` picks every script in the map's folder, including its interiors and sub-scripts, and an override must find its function in each; use `script` with a `?` or `*` pattern to pick fewer. A `.c` file without `#pragma target` is a shared header/library. |
| `#pragma override` | Replace a function in the target script. Call `original(...)` to run the replaced function. |
| `#pragma override <native_name>` | Wrap an engine native function (such as `place_monster`) across the target script. |
| `#pragma every_frame` | Run the `void func(void)` function after it once per frame, before every `WAIT();` in the target script. The function must not `wait` itself. |
| `PROGRAM(id, func)` | Register a new event program entry point in the target script. |

Example native override:

```c
#pragma override place_monster
void place_rats(monster_kind monster, float x, float y, float z, float rx, float ry, float rz, int talk)
{
    if (monster == MONSTER_GRIFFON_SOLDIER_523) {
        monster = MONSTER_SEWER_RAT;
    }
    original(monster, x, y, z, rx, ry, rz, talk);
}
```

## Numbers of a mod's own

Do not hardcode save flag numbers or event program IDs. Declare them with these macros:

| Macro | Range | Purpose |
| --- | --- | --- |
| `NEW_FLAG(NAME)` | `1024`–`2046` | Allocate an unused save flag |
| `NEW_COUNTER(NAME)` | `32`–`127` | Allocate an unused 16-bit save counter |
| `NEW_PROGRAM(NAME)` | `9000`–`9999` | Allocate an unused map event program number |
| `NEW_MONSTER_VAR(NAME)` | `8`–`135` | Allocate an unused floor-wide monster variable |

`dc2tk build` stores assigned numbers in `mods/numbers.json` so IDs stay stable and do not collide across mods.

## Text in each language

Each PAL map script has five language variants (`_1.c` English, `_2.c` French, `_3.c` German, `_4.c` Italian, `_5.c` Spanish). A patch with `#pragma target map s51` compiles into all five variants. `dc2tk mod override` copies a function's text in every language as `#if LANGUAGE` branches: keep them, or players of the other languages see your English text. Use `LANGUAGE` to branch by language:

```c
#if LANGUAGE == LANGUAGE_FRENCH
    modkit_say("Bonjour !");
#else
    modkit_say("Hello!");
#endif
```

## Mods that build on mods

| `mod.json` field | Behavior |
| --- | --- |
| `"requires": ["base-mod"]` | Automatically includes `base-mod`, applies `base-mod` first, and lets patches `#include "base-mod/header.h"`. |
| `"after": ["other-mod"]` | Applies your mod after `other-mod` when both are built together, without requiring it. |

## Whole files

To replace an entire script file instead of using a patch, place it under `mods/<name>/files/` at its path in the tree (or run `dc2tk mod new extracted/scripts <name> map/s/s51_raybrandt_castle/s51_1.c`). Run `dc2tk mod slim extracted/scripts <name>` to convert full-file copies into function patches.

To replace or add a raw disc file (such as a `.chr` or `.img` archive built from a template), use `"file:<disc-path>"` in `mod.json`:

```json
{
  "add": {
    "file:mods/calculator/calc_ui.img": {"file": "ui", "like": "texture:logo_d00"}
  }
}
```

## Trying a change quickly

Set `"start_at"` in `mod.json` (or pass `--start-at` to `dc2tk build`) to make **New Game** warp directly to your map or event:

| Value | Result on New Game |
| --- | --- |
| `"m01"` | Spawn in Palm Brinks at `map_start` |
| `"s51:story_50"` | Run program `story_50` (`10500`) on map `s51` |
| `"s51:MY_PROGRAM"` | Run `NEW_PROGRAM(MY_PROGRAM)` on map `s51` |
| `"s06/s06b:500"` | Run program `500` of map `s06`'s script `s06b` |

`dc2tk build` prints how the new game gets there, and warns when the game never enters the map that way (so the warp may not work). `dc2tk map extracted/scripts s51` lists a map's programs. Pass `--no-start-at` to build a mod for a normal New Game.

## Hot reloading in the native port

The native port's debug build (`python3 -I tools/port.py --movies --debug`) can pick up a mod's
changes while the game runs. Start the game, then watch the mod:

```sh
dc2tk mod watch extracted/scripts dc2.iso my-mod
```

`mod watch` builds the mod as `dc2tk build` would, but instead of an image it writes the game
files the mod changes into `build/port/overrides/` (by their path on the disc, for example
`map/s/s42/s42_1.stb`). Whenever a file in the mod's folder changes it rebuilds, writes only the
files that changed, removes the ones the mod no longer changes, and tells the game to reload
through its API (`http://127.0.0.1:7777/reload`, or `$DC2TK_DEBUG_PORT`). Pass `--once` to build
and reload once, and `--overrides DIR` (or set `DC2TK_OVERRIDES` for both) to use another folder.

The port reads a file from the override folder instead of the disc whenever one is there, at any
size. On a reload it rescans the folder and re-enters the current town map with the player where
they stood, so scripts, maps, models and textures load again; in a dungeon it re-enters the
floor. `reload?event=N` (or `rerun=1`, which re-runs the event in progress) starts an event
instead. The debug window's **Reload** button does the same; **auto** lights up while
`mod watch` is running.

Engine files and executable table edits are not reloaded: rebuild the port for those. The
override folder stays in use after `mod watch` stops, so delete `build/port/overrides/` to play
the disc as it is.

## The mod commands

See [CLI Reference: Creating and managing mods](commands.md#creating-and-managing-mods) for all `dc2tk mod` subcommands (`make`, `asset`, `override`, `new`, `save`, `check`, `test`, `list`, `slim`, `apply`, `header`, `watch`).

## Tests

Create `mods/<name>/tests.c` to test C helper functions without starting PCSX2. Natives do nothing in the test VM and return `0`, so test your own functions rather than the game's:

```sh
dc2tk mod test extracted/scripts <name>
```

### A first test

Every `int test_...(void)` function in `tests.c` runs in the built-in script VM. Return non-zero to pass or `0` to fail:

```c
#include <event.h>
#include "my_math.c"

int test_addition(void)
{
    return add_two(2, 3) == 5;
}
```

## Engine files

A mod can replace functions in the game's executable. Put C++ files under its `engine/`
folder. Files named after units in ChronicleTwo's `engine/ps2/src/` extend those units; a new
filename adds a unit. Mark each replacement with
`PORT_OVERRIDE` and keep its function name and parameter types. An unmarked function adds a
new definition to that unit.

```text
mods/my-mod/
  mod.json
  engine/
    userdata.cpp
```

Include the engine headers that declare the types your code uses. Code must compile with
MWCC 3.0, so use the C++ supported by that compiler. Functions and data should use engine
symbols; fixed RAM addresses and hardware access will prevent the same mod running in the
native port.

```sh
dc2tk mod check extracted/scripts my-mod --disc dc2.iso
dc2tk build extracted/scripts dc2.iso -o modded.iso --mod my-mod
```

Both commands build the engine in Docker or Podman. The disc supplies retail's libraries;
`mod check` therefore needs `--disc` for engine mods. A build with `--dry-run` compiles the
engine too. Mods that replace one function clash and the error names both mods and the
function. `--executable` cannot be combined with engine files.

Each mod set gets a copy under `build/engine/mods-<hash>/`, with its engine inputs included in
the hash. Builds edit that copy and leave the engine submodule untouched. The link map is
`build/pal/SCES_511.90.xMAP` within the copy. Every build reports the C library heap left below
the PS2 stack, warns if it is smaller than retail's 112128 bytes, and fails if BSS overlaps
the stack. Try the resulting image in PCSX2 before distributing it.

Call `original(...)` inside a marked function to run the engine's previous implementation.
The prepared copy keeps that function under a new name; member declarations are added to the
copied class header. This supports ordinary functions and methods on an unqualified class.
Constructors, destructors, operators and nested or namespace-qualified classes are refused
when they need `original()`.

An existing `.h` or `.hpp` under `engine/ps2/include/` can be replaced by putting the whole
header at the same relative path in the mod's `engine/` folder. Two mods replacing one header
clash. A `.cpp` with a new filename adds a translation unit and its linker sections. These
files are compiled with the rest of the engine.

To start an override from the engine's function:

```sh
dc2tk mod override engine my-mod 'CUserDataManager::AddMoney(int)'
```

The command copies the function into the mod's `engine/` folder with `PORT_OVERRIDE` added.
It refuses an existing destination; add required includes before compiling. A bare function
name works when it selects only one definition. The engine checkout stays untouched.

Engine mods can be combined with edits to executable data tables. Their edited values are
written into the prepared source initializers before compiling. If engine code also changes
the initializer of a table edited through JSON, the build refuses the overlap. Function changes
that leave that initializer intact can be combined with its JSON edits. Mods without engine
files keep the existing executable byte patches and need no container.

Native tables and the SDK declarations used for script compilation are regenerated from the
prepared engine. A mod can replace a native table using a marked global definition and add
its function in the same unit. Its script patches then compile against the generated native
numbers. The generated headers are under `build/mod-sdk/include/` within the prepared copy;
the installed SDK is kept.
