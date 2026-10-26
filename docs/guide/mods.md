# Creating Mods

A mod is a folder inside `mods/` (for example, `mods/castle/`) with a `mod.json` that gives it an **id** such as `andrew.castle`. Storing changes in `mods/` keeps `extracted/` unmodified so you can combine multiple mods at build time, choosing each by its id:

```sh
dc2tk build extracted/scripts dc2.iso -o modded.iso --mod andrew.castle --mod someone.another-mod
```

See four complete example mods in `docs/scripting/examples/` (copy one into `mods/` to build it):

- **[calculator](../scripting/examples/calculator/calculator.c)**: In-game calculator with a custom 3D model, 2D UI sprites, and sound effects.
- **[spinning-cult](../scripting/examples/spinning-cult/README.md)**: Quest mod with a custom Blender room, a door in Palm Brinks, a custom NPC, a custom item, and new music.
- **[hard-mode](../scripting/examples/hard-mode/README.md)**: Tougher monsters, fragile weapons and pricier shops, made only of data tables, with settings and mod sets that choose them.
- **[boss-rush](../scripting/examples/boss-rush/README.md)**: Six of the game's boss fights back to back from the Palm Brinks dungeon entrance, timed, with a best time in the save.

## Mod folder layout and `mod.json`

A mod folder contains `mod.json` alongside any `.c` script patches and asset files:

```text
mods/my-mod/
├── mod.json            # The mod's id and version, and the assets and data tables it replaces and adds
├── mod.lock            # Written by dc2tk (whole files the mod copied); never edit it
├── s51.c               # Script patch (optional)
├── data/
│   └── items.json      # Edited item stats (optional)
├── art/
│   └── soldier.png     # Replacement texture (optional)
├── files/              # Whole script files the mod copied (optional; see Whole files)
└── raw/                # Game files copied off the disc to edit (optional; see Game files)
```

Only `mod.json` has a fixed name. Every other file is found through it: the paths its lines name, and the script sources (every `.c` and `.h` outside `files/`, or the ones `"scripts": {"sources"}` picks).

Example `mod.json`:

```json
{
  "format": 2,
  "id": "andrew.my-mod",
  "version": "1.0.0",
  "name": "My Mod",
  "description": "Tougher soldiers in the castle.",
  "authors": ["Andrew"],
  "dependencies": {"someone.other-mod": "^1.2"},
  "replace": {
    "texture:e254a1": "art/soldier.png",
    "sound:SE_CURSOR_MOVE": "sounds/click.wav",
    "music:BG_000_000": "music/title.mid",
    "model:e33a": "models/e33a",
    "map:s17": "maps/s17/s17.gltf"
  },
  "add": {
    "sound:SE_MY_BEEP": {"bank": "SP_042", "file": "sounds/beep.wav"},
    "music:BG_201": {"file": "music/theme.mid", "like": "BG_193"},
    "map:fishroom": {"file": "maps/fishroom/fishroom.gltf", "like": "m07", "title": "Fish Room", "script": "fishroom.c"}
  },
  "data": {
    "items": "data/items.json",
    "monsters": "data/monsters.json"
  },
  "text": {
    "en/shopname": "text/en/shopname.txt"
  },
  "files": {
    "mod:banner.img": {"file": "art/banner", "like": "texture:logo_d00"}
  },
  "settings": {
    "hard": {"type": "bool", "default": false, "label": "Tougher soldiers"}
  },
  "dev": {"start_at": "s51:story_50"}
}
```

`dc2tk mod new` writes a `mod.json` with `"format"`, `"id"` and `"version"` for you (`--id andrew.my-mod`, or `local.<folder>` when you leave it out). Every key is checked: a key dc2tk does not know, or a misspelt one, is an error rather than being ignored. The schema is `definitions/mod.schema.json`: add `"$schema": "../../definitions/mod.schema.json"` to a `mod.json` in `mods/` and an editor that reads JSON schemas completes and checks it as you type.

| Key | Meaning |
| --- | --- |
| `"format"` | Always `2`. |
| `"id"` | The mod's name everywhere: `author.name`, lowercase letters, digits and `-`. It never changes, even if the folder is renamed. |
| `"version"` | `MAJOR.MINOR.PATCH`, as `1.0.0`. |
| `"name"`, `"description"`, `"authors"`, `"license"`, `"homepage"` | For people; `dc2tk mod list` shows the name and version. |
| `"game"` | The disc the mod is for; `"dc2-pal"`, the default, is the only one today. |
| `"tool"` | The dc2tk versions the mod needs, as a range (`">=0.1"`). |
| `"targets"` | Where the mod runs: `["iso", "native"]` (the default) promises it works the same on a PS2 or PCSX2 image and on the native port. `["native"]` marks a mod that uses port-only features; it can't go into an image. |
| `"dependencies"`, `"after"`, `"conflicts"` | Mods this one builds on, is applied after, or can't be built with (see [Mods that build on mods](#mods-that-build-on-mods)). |
| `"replace"` | Game assets the mod changes, by id. A line is a file path, or an object with `"file"` and options. |
| `"add"` | New assets: sound effects, music files and maps. |
| `"data"` | Data tables the mod changes, by table name (`"items"`): a JSON file, or the changed records written in place as a list (see [Game Data](data.md)). |
| `"text"` | Text files the mod changes, by id (`"en/shopname"`): a `.txt` file, or the changed messages in place as `{"12": "New text"}`. |
| `"files"` | Game files by their path on the disc, and the mod's own new files as `mod:NAME` (see [Game files](#game-files)). |
| `"scripts"` | `"sources"`: the patches and headers to use, as patterns (`["patches/*.c"]`); every `.c` and `.h` outside `files/` when left out. `"whole"`: whole script files (see [Whole files](#whole-files)). `"additions"`: programs, includes and wrappers added to scripts the mod does not copy. |
| `"settings"` | Choices a player makes when building the mod (see [Settings](#settings)). |
| `"port"` | Code and settings for the native port (see [Native port mods](#native-port-mods)). |
| `"dev"` | Settings for making the mod, never for players: `"start_at"`, where **New Game** starts in a `--dev` build (see [Trying a change quickly](#trying-a-change-quickly)). |

Options a line can take, besides `"file"`:

| Option | Lines | Meaning |
| --- | --- | --- |
| `"only"` | `texture:`, `model:`, `sound:` | Replace the asset only under the disc paths listed (for example, `["map/a"]`) |
| `"volume"` | `sound:` | The sound's volume |
| `"bank"` | `sound:` under `"add"` | The bank to add the sound to |
| `"instruments"` | `music:` | WAV files as new instruments (see [Music](music.md)) |
| `"like"` | `music:`, `map:` under `"add"`; `"files"` | The existing asset to copy as a template |
| `"title"`, `"script"`, `"compact"` | `map:` under `"add"` | The new map's title, its C script, and whether deleted placements are dropped (see [Maps](maps.md)) |
| `"programs"` | `map:` | Script programs for the map's event points, by point name (see [Maps](maps.md)) |
| `"when"` | every line | Use the line only when a true/false setting is on (`"hard"`), or off (`"!hard"`) |

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

`dc2tk build` stores assigned numbers in `mods/mods.lock` (or a mod set's lock file, see [Mod sets](#mod-sets)), keyed by each mod's id, so they stay stable from build to build and never collide across mods. A save made with a set of mods reads the same flags after you add another mod.

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
| `"dependencies": {"andrew.base": "^1.0"}` | Automatically includes `andrew.base` (refusing a version outside the range), applies it first, and lets patches `#include "andrew.base/header.h"`. |
| `"after": ["someone.other"]` | Applies your mod after `someone.other` when both are built together, without needing it. |
| `"conflicts": {"someone.fish-overhaul": "*"}` | Refuses to build together with those versions of `someone.fish-overhaul`. |

Version ranges are `*` (any), `^1.2` (1.2.0 up to, not including, 2.0.0), `~1.2.3` (1.2.3 up to 1.3.0), `=1.2.3`, or comparisons joined by commas (`>=1.0, <2.0`).

## Whole files

To replace an entire script file instead of using a patch, run `dc2tk mod new extracted/scripts <name> map/s/s51_raybrandt_castle/s51_1.c`. It copies the file to `files/map/s/s51_raybrandt_castle/s51_1.c` and lists it in `mod.json`:

```json
"scripts": {
  "whole": {"map/s/s51_raybrandt_castle/s51_1.c": "files/map/s/s51_raybrandt_castle/s51_1.c"}
}
```

The key is the file's path in the tree and the value is the copy in the mod. `dc2tk mod save` lists the files it copies the same way. Run `dc2tk mod slim extracted/scripts <name>` to turn whole files into function patches where it can: it drops each copy it no longer needs from `"whole"`.

## Game files

`"files"` replaces a game file by its path on the disc, or adds one. Run `dc2tk mod asset dc2.iso my-mod chara/c02a.chr --as gltf` to copy a game file into the mod's `raw/` folder (here as glTF) and add its line:

```json
"files": {
  "chara/c02a.chr": "raw/chara/c02a.chr.gltf"
}
```

A mod's own new files are named `mod:NAME`. On the disc they go in the mod's own folder, `mods/<id>/NAME`, so two mods never pick the same path. Give the file to copy as a template with `"like"`:

```json
"files": {
  "mod:calc_ui.img": {"file": "ui", "like": "texture:logo_d00"},
  "mod:calc.chr": {"file": "model/calc.gltf", "like": "model:uki"}
}
```

Scripts load them through `MOD_DIR`, the mod's folder on the disc (`"mods/<id>/"`):

```c
load_chara(MEMORY_STACK_EVENT, MOD_DIR "calc.chr", "info.cfg", KIOSK_SLOT);
```

A line without `mod:` and with `"like"` adds a file at a path of the game's (`"item/gyros.chr"`).

## Settings

`"settings"` declares choices a player makes when they build the mod. Each has a `"type"`: `"bool"`, `"int"` (with `"min"` and `"max"`), `"enum"` (with `"values"`) or `"string"`, a `"default"`, and a `"label"` for people:

```json
"settings": {
  "hard": {"type": "bool", "default": false, "label": "Tougher soldiers"},
  "lives": {"type": "int", "default": 3, "min": 1, "max": 9}
}
```

Patches and the mod's map scripts see each one as `SETTING_<name>`: `1` or `0` for a bool, the number for an int, the index in `"values"` for an enum, and the text for a string:

```c
#if SETTING_hard
    modkit_say("No mercy this time.");
#endif
    int lives = SETTING_lives;
```

A line with `"when": "hard"` is used only when `hard` is on, and `"when": "!hard"` only when it is off. A `--mod` build uses every default; a [mod set](#mod-sets) chooses other values.

## Sharing a mod

Pack a mod into a single file to share it:

```sh
dc2tk mod pack andrew.my-mod
```

This writes `andrew.my-mod-1.0.0.dc2mod`, a zip of the files `mod.json` names, the script sources, `engine/` and `port/`, `mod.lock`, and any `README`, `LICENSE` or `CHANGELOG`. The `"dev"` section is left out. Packing the same mod twice gives the same bytes. Pass `--disc dc2.iso` to refuse any file that is one of the disc's own, so a zip never ships the game's data.

Install a `.dc2mod` into the mods folder with:

```sh
dc2tk mod install andrew.my-mod-1.0.0.dc2mod
```

An installed zip is used where it is, by its id, like a folder: `dc2tk mod list` shows both. Installing a newer version replaces the older zip. A zip can't be installed over a folder mod with the same id.

## Playing built mods

A `.dc2mod` is a mod's sources; players of the native port need the game files a mod builds to. Build them with:

```sh
dc2tk mod build extracted/scripts dc2.iso andrew.my-mod -o build/port/mods
```

This writes `andrew.my-mod-1.0.0.dc2pak`: an uncompressed zip of `pak.cfg` (the mod's id, name, version, description, authors, the save flags, counters and programs it was given, and the game files it changes) and those files under `files/`, including its port settings and HD textures. Mods it depends on are built into it. A mod with engine or port code, or that changes the executable's tables, needs its own game binary and is refused; play it with `dc2tk play --native --set`.

Put `.dc2pak` files in the `mods` folder beside the game (for `Dark Chronicle.app`, `~/Library/Application Support/DC2 Toolkit/Dark Chronicle/mods`; on iOS, the `mods` folder in the app's folder in Files), or drop one on the launcher. The launcher's Mods page turns each one on and off, and the game reads their files straight from the zips. Built mods replace whole game files, so two mods that change the same file, or were given the same number, can't both be on: the launcher won't turn the second on and says which mod and which file it clashes with. Files in the override folder (`dc2tk mod watch`, the Blender add-on's Play in Game) still come first. `--mods ID,ID` turns built mods on when the game is started with a disc and no launcher.

Built mods go on disc images too. `--pak` adds one to a build, and can be given more than once:

```sh
dc2tk build extracted/scripts dc2.iso -o modded.iso --mod andrew.my-mod --pak someone.other-mod-1.2.0.dc2pak
```

A pak can't change a file, or use a number, that a mod built from source or another pak does; the build stops and names both. Its port settings and HD textures stay off the image, since only the native port reads them.

### Upgrading an old mod

A mod from before format 2 (with `"title"`, `"requires"`, `"start_at"`, a `disc/` folder, and its numbers in `mods/numbers.json`) can be upgraded in place:

```sh
dc2tk mod upgrade my-old-mod --id andrew.my-old-mod
```

This rewrites its `mod.json` as format 2: `"title"` becomes `"name"`, `"requires"` becomes `"dependencies"`, `data:`, `text:` and `file:` entries move to `"data"`, `"text"` and `"files"` (`file:mods/<folder>/NAME` becomes `mod:NAME`), `"additions"` moves under `"scripts"`, whole files in `files/` and `disc/` files get their lines, and `"start_at"` moves under `"dev"`. The record of whole files goes to `mod.lock`, and the mod's numbers move from `numbers.json` to `mods.lock` under its id, so saves keep working. Its `.c` and `.h` files load their new files through `MOD_DIR` and include other mods' headers by id. Without `--id` the id is `local.<folder>`. After that, `mod build` makes a `.dc2pak` of it like any other mod.

## Mod sets

A mod set is a JSON file that chooses mods, their versions and their settings:

```json
{
  "format": 2,
  "mods": [
    {"id": "andrew.my-mod", "version": "^1.0", "settings": {"hard": true}},
    {"id": "someone.other-mod"}
  ]
}
```

Build it with `--set` in place of `--mod`:

```sh
dc2tk build extracted/scripts dc2.iso -o modded.iso --set my-game.json
```

The set's numbers and the versions it was built with are kept in `my-game.lock` beside it, so a set keeps its save flags apart from every other set's. A version outside the set's range, or a setting the mod does not have, is refused.

Play a set on the native port without writing an image:

```sh
dc2tk play dc2.iso --native --set my-game.json
```

This writes the set's game files into `build/sets/my-game/overrides/` and starts the port over the retail disc with them. A set with engine or port code builds its own port binary in that folder first. Add `--build-only` to build and print the command instead of starting the game.

## Native port mods

A mod with `"targets": ["native"]` may use features only the native port has. Such a mod can't go into a disc image.

**HD textures.** An `hd:` line draws a texture at a higher resolution on the port, from a PNG a whole multiple of its size:

```json
"replace": {
  "hd:texture:i01h0101": "hd/i01h0101.png"
}
```

**Port settings.** `"port": {"settings"}` sets the port's own options:

| Setting | Values | Port option |
| --- | --- | --- |
| `render.aspect` | `"16:9"`, `"21:9"` and so on: towns and dungeons widen to it, menus and the title stay 4:3 | `--aspect 16:9` |
| `render.scale` | `1` to `4`, or `"auto"` to render at the window's resolution | `--scale` |
| `render.msaa` | `1`, `2`, `4` or `8` samples | `--msaa` |
| `render.window` | the window's size, `"1920x1080"` | `--window 1920x1080` |
| `render.fullscreen` | `true` or `false`; Alt+Enter or F11 toggles it while playing | `--fullscreen` |
| `render.vsync` | `true` waits for the display's refresh | `--vsync` |
| `render.filter` | `"nearest"` for sharp pixels, else smooth | |
| `render.smooth` | `false`, `"display"` to draw frames between the game's updates (below) at the display's rate, or a rate such as `144` | `--smooth 144`, `--smooth 0` |

```json
"port": {"settings": {"render.aspect": "16:9", "render.scale": "auto"}}
```

The port's options win over these, and so do the choices made in its launcher.

**The display mod.** `port/display/` is a mod, `dc2tk.display`, that gives players these choices as settings of their own (aspect, window size, fullscreen, vsync, render scale, multisampling, filter) and sets them through `port_mod.h`. Link or copy the folder into the mods folder and choose it in a [mod set](#mod-sets):

```json
{"format": 2, "mods": [{"id": "dc2tk.display", "settings": {"aspect": "21:9", "window": "2560x1440"}}]}
```

**The smooth motion mod.** The game updates 25 times a second in towns and dungeons (50 at the title), and everything in it moves one step per update. `port/smooth/` is a mod, `dc2tk.smooth`, that draws frames in between for 60, 120 or 144 Hz displays: the port records each frame it draws and, while the game waits for its next update, draws the recording again with the camera, every model and every bone blended from the update before to the last. Particles, 3D sprites, water and the HUD keep the game's own rate, and the picture is one update (40 ms) behind. Its settings are `enabled` and `refresh` (`display`, the screen's rate, or 60 to 240). The port's debug build shows the frames it draws between updates as `replays` in `stats`, and `between alpha=0.5` returns one as a picture.

**Port code.** `"port": {"sources", "api"}` adds C or C++ files to the port. `"sources"` picks them with patterns and `"api"` is the range of `port_mod.h` they are written for (`"^1.2"`). Each file includes `port_mod.h` and registers a table of hooks:

```c
#include "port_mod.h"

static void every_field_frame(void)
{
}

static const PortMod hello = {.id = "andrew.hello", .field = every_field_frame};
PORT_MOD(hello)
```

The hooks are `start`, `event`, `field`, `frame_enter`, `frame_drawn`, `frame_exit`, `overlay`, `present`, `input` and `audio`. `port_setting("andrew.hello.speed")` reads a setting's value as text. API 1.1 adds the display: `render_set_aspect`, `render_set_window`, `render_set_vsync`, `render_set_quality` (`RENDER_SCALE_AUTO` for the window's resolution), `render_set_filter`, `render_toggle_fullscreen`, and `render_display` and `render_frame_width` to read them back. API 1.2 adds smooth motion: `render_set_smoothing`, `render_smoothing` and `smooth_set_rate`. API 2.0 drops the `host` hook. In the debug build, `port_mod_command` adds a command to the debug API and `port_mod_panel` adds a panel to the debug window's **Mods** tab. The debug tools are themselves a port mod, `dc2tk.debug-tools`, in `port/debug/`. Build the port with another mod's code using `python3 -I tools/port.py --mod mods/hello`.

## Trying a change quickly

Set `"dev": {"start_at": ...}` in `mod.json` and build with `--dev` (or pass `--start-at` to `dc2tk build`) to make **New Game** warp directly to your map or event. Without `--dev`, a build is a normal New Game, so a player's build never warps:

| Value | Result on New Game |
| --- | --- |
| `"m01"` | Spawn in Palm Brinks at `map_start` |
| `"s51:story_50"` | Run program `story_50` (`10500`) on map `s51` |
| `"s51:MY_PROGRAM"` | Run `NEW_PROGRAM(MY_PROGRAM)` on map `s51` |
| `"s06/s06b:500"` | Run program `500` of map `s06`'s script `s06b` |

`dc2tk build` prints how the new game gets there, and warns when the game never enters the map that way (so the warp may not work). `dc2tk map extracted/scripts s51` lists a map's programs. Leave out `--dev` to build a mod for a normal New Game.

## Hot reloading in the native port

The native port's debug build (`python3 -I tools/port.py --movies --debug`) can pick up a mod's
changes while the game runs. Start the game, then watch the mod:

```sh
dc2tk mod watch extracted/scripts dc2.iso local.my-mod
```

`mod watch` builds the mod as `dc2tk build` would, but instead of an image it writes the game
files the mod changes into `build/port/overrides/` (by their path on the disc, for example
`map/s/s42/s42_1.stb`). Whenever a file in the mod's folder changes it rebuilds, writes only the
files that changed, removes the ones the mod no longer changes, and tells the game to reload
through its API (`http://127.0.0.1:7777/reload`, or `$DC2TK_DEBUG_PORT`). Pass `--once` to build
and reload once, and `--overrides DIR` (or set `DC2TK_OVERRIDES` for both) to use another folder.
`--reload "map=m01&x=100&y=0&z=-200&yaw=1.57"` adds to the game's reload request: it enters that
town map with the player at that spot (game axes; yaw in radians) instead of where they stand.
The Blender add-on's **Play in Game** runs `mod watch --once` this way (see [Maps](maps.md)).

The port reads a file from the override folder instead of the disc whenever one is there, at any
size. On a reload it rescans the folder and re-enters the current town map with the player where
they stood, so scripts, maps, models and textures load again; in a dungeon it re-enters the
floor. `reload?event=N` (or `rerun=1`, which re-runs the event in progress) starts an event
instead, and `reload?map=NAME&x=&y=&z=&yaw=` enters a town map at a spot, from the title too. The debug window's **Reload** button does the same; **auto** lights up while
`mod watch` is running.

Engine files and executable table edits are not reloaded: rebuild the port for those. The
override folder stays in use after `mod watch` stops, so delete `build/port/overrides/` to play
the disc as it is.

## The mod commands

See [CLI Reference: Creating and managing mods](commands.md#creating-and-managing-mods) for all `dc2tk mod` subcommands (`make`, `asset`, `override`, `new`, `save`, `check`, `test`, `list`, `slim`, `apply`, `header`, `watch`, `pack`, `install`).

## Tests

Create `mods/<name>/tests.c` to test C helper functions without starting PCSX2. Natives do nothing in the test VM and return `0`, so test your own functions rather than the game's:

```sh
dc2tk mod test extracted/scripts local.my-mod
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
dc2tk mod check extracted/scripts local.my-mod --disc dc2.iso
dc2tk build extracted/scripts dc2.iso -o modded.iso --mod local.my-mod
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
