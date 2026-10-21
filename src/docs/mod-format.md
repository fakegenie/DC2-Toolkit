# Mod format 2: a review and a proposal

A proposal, not yet built. It reviews how mods are made, built and shared today, then
proposes one `mod.json` per mod, a zip to share it in, and a format that can grow without
breaking: everything from engine code to scripts, maps, textures, models and sounds, and
the native port itself. It does not keep format 1 working; `dc2tk mod upgrade` converts a
format 1 folder once.

## 1. How it works today

A mod is a folder under `mods/`, named by the folder. `mod.json` maps asset ids to files
(`replace`, `add`), and the rest of the mod sits beside it in several other forms:

| What | Where | Read by |
| --- | --- | --- |
| metadata, `requires`, `after`, `start_at` | `mod.json` | `mods.rs`, `startat.rs` |
| assets by id, raw files by disc path, maps, new music | `mod.json` `replace` and `add` | `replace.rs` |
| data table edits | `data/*.json`, one file per table, named by a `data:` line | `datatables.rs` |
| script patches and libraries | any `*.c` and `*.h` outside `files/` and `disc/` | `patches.rs` |
| programs, includes and wraps added to scripts | `mod.json` `additions` | `additions.rs` |
| whole script files | `files/`, indexed by `mod.json` `files` and `includes` | `mods.rs` |
| raw game files | `disc/` (by extract path) or `raw/` with `file:` lines | `discfiles.rs` |
| engine C++ | `engine/`, units and replaced headers | `engine.rs`, `tools/engine_mods.py` |
| save flags, counters, programs, monster variables | `mods/numbers.json`, one for the whole folder | `numbers.rs` |
| tests | `tests.c` | `mods.rs` |
| editor files | `.editor/`, `compile_commands.json`, `files/compile_flags.txt` | `editor.rs` |

What works well and should stay:

- **Assets by id.** One id reaches every place the disc has the asset, each built with its
  own place as the template. Common formats in (PNG, glTF, WAV, MIDI, text), and clashes
  between mods refused with both mods named.
- **Patches instead of copies.** Script patches with `#pragma override` and `original()`,
  data tables as partial records merged across mods, engine overrides with `PORT_OVERRIDE`:
  several mods can touch one script, table or unit.
- **The target lives in the source** (`#pragma target map s51`), so a patch says what it
  patches.

What gets in the way:

1. **No format.** `mod.json` is read as untyped JSON with no version field. Unknown
   top-level keys are ignored, so a typo in `requires` or a key from a newer tool does
   nothing, while an unknown line option is an error. The docs and the code disagree on which
   lines take `only`.
2. **Identity is the folder name.** Rename the folder and every `requires`, every
   `numbers.json` key and every `#include "base-mod/..."` breaks. There is no
   author-independent id, no version range on a dependency, and no `conflicts`.
3. **One mod is many files in many layouts.** It is spread over `mod.json`, one JSON per data
   table, `additions`, `files/`, `disc/`, `raw/`, `engine/` and the shared `numbers.json`.
   There are three ways to ship a raw game file (`disc/`, `raw/` plus `file:`, `files/`).
4. **Tool state sits in the author's file.** `files`, `includes` and `note` are written into
   `mod.json`, and `write_entries` re-sorts the author's keys.
5. **Numbers belong to the folder, not to what the player chose.** Mods already name their flags,
   counters, programs and monster variables (`NEW_FLAG(name)`) and the build picks the numbers,
   but `mods/numbers.json` holds every assignment for the whole folder, keyed by folder name. A
   player's saves depend on a file that is not part of their mod set, and that file
   breaks when a folder is renamed.
6. **New files have no namespace.** A mod's own files are `file:mods/calculator/...` by
   convention only; two mods can pick one path.
7. **Development settings ship with the mod.** `start_at` changes New Game and has to be
   turned off with `--no-start-at`.
8. **Nothing to share.** There is no pack, install or zip. A mod is a folder you copy, and
   building one needs the decompiled tree (`extracted/scripts`), the disc, and Docker for
   engine code.
9. **Two targets that drift.** `build -o x.iso` applies everything. `build --native` builds
   the binary but writes no game files, so a native player gets scripts and assets only
   through `-o` or the debug build's override folder. `mod watch` cannot carry engine or
   executable changes.
10. **The port cannot be modded.** `engine/` takes only `ps2` units and headers; nothing
    reaches `port/`. The debug build is already an extension in all but name (it plugs in
    through the weak hooks in `port/include/port_debug.h`), but only the repository can
    supply one.
11. **Paths relative to two places.** `--mods` is relative to the working folder, while
    `build/` and `engine/` are relative to the repository.

## 2. What format 2 is for

- One `mod.json` holds everything that is not an asset file or code: metadata,
  dependencies, asset and file mappings, data table edits, text edits, script additions,
  settings, and port options.
- A mod is a folder while it is made and a zip when it is shared. Both are read the same
  way, and a zip is used without unpacking it.
- Every layer can be changed: the engine, scripts, maps, textures, models, motions, sounds,
  music, text, data tables, raw game files, and the native port's own code and settings.
- It can grow. A version number says what a file means. Unknown keys are errors, so typos
  are caught. New kinds of content are added as new keys and id kinds, never by changing what
  an old key means.
- A shared mod holds only the author's work. The game's own bytes come from the player's
  disc when the mod is built.

## 3. The package

```text
spinning-cult/                  or spinning-cult-1.2.0.dc2mod (a zip of the same files)
├── mod.json                    everything that is not an asset or code
├── mod.lock                    written by dc2tk, shipped, never edited by hand
├── assets/                     the mod's files: PNG, glTF, WAV, MIDI, text, raw game files
├── scripts/                    script patches and libraries (*.c, *.h), tests.c
├── engine/                     engine C++ (units, replaced headers)
└── port/                       native port C/C++ against the port's mod API
```

Only `mod.json` is fixed. The folder names are defaults that `mod.json` can change. Every file
in the package is named by `mod.json`, either directly or through a folder or glob it lists, so
`dc2tk mod pack` knows exactly what to ship. Editor files (`.editor/`, `compile_commands.json`)
are never packed.

The zip:

- The file is named `<id>-<version>.dc2mod`. It is an ordinary zip (stored or deflate), with
  `mod.json` at the root or inside one top folder.
- Names are UTF-8 and use `/`. Absolute paths, `..`, symbolic links, and two names that differ
  only in case are refused, so a mod unpacks the same way on Windows and macOS.
- `mod pack` writes members sorted, with fixed timestamps, so one mod always gives the same
  bytes and its hash can identify it. `mod.json` can hold a `"sha256"` per dependency for
  people who want to pin one exactly.
- `mod pack` refuses a file whose bytes are a game file or asset from the disc (it checks the
  disc index when one is available). Patches may hold whole decompiled functions, as
  `mod override` writes them. Whole script files are not shipped: several mods can patch one
  script, but two copies of it cannot be combined.

The mods folder takes folders and `.dc2mod` files side by side. `dc2tk mod install x.dc2mod`
copies a zip in, `mod list` shows both kinds, and nothing needs unpacking.

## 4. mod.json

```json
{
  "format": 2,
  "id": "andrew.spinning-cult",
  "version": "1.2.0",
  "name": "The Spinning Cult",
  "description": "A quest in Palm Brinks: a fish room, a cult and their song.",
  "authors": ["Andrew"],
  "license": "CC-BY-4.0",
  "game": "dc2-pal",
  "tool": ">=2.0",
  "targets": ["iso", "native"],

  "dependencies": {"andrew.npc-kit": "^1.0"},
  "after": ["someone.palm-brinks-lights"],
  "conflicts": {"other.fish-overhaul": "*"},

  "replace": {
    "texture:itemicon": "assets/itemicon.png",
    "model:c02fuku*": "assets/monica/c02b.gltf",
    "sound:SE_CURSOR_MOVE": {"file": "assets/click.wav", "volume": 90},
    "map:m01": {"file": "assets/maps/m01/m01.gltf", "programs": {"door_fish": "ENTER_FISH_ROOM"}}
  },
  "add": {
    "map:fishroom": {"file": "assets/maps/fishroom.gltf", "like": "m07", "title": "The Fish Room",
                     "script": "scripts/fishroom.c", "programs": {"door_out": "LEAVE_FISH_ROOM"}},
    "music:BG_201": {"file": "assets/turning.mid", "like": "BG_193",
                     "instruments": {"100": {"file": "assets/musicbox.wav", "root": 84}}},
    "sound:SE_CULT_CHANT": {"file": "assets/chant.wav", "bank": "SP_042"},
    "model:gill": {"file": "assets/gill.gltf", "like": "model:p01_01a"}
  },
  "files": {
    "item/gyros.chr": {"file": "assets/gyros_item.gltf", "like": "model:takaraky"}
  },

  "data": {
    "items": [
      {"item": 429, "like": 1, "names": {"en": "Gyros", "fr": "Gyros"}, "weapon": {"attack": 40}}
    ],
    "inventions": "assets/inventions.json"
  },
  "text": {
    "en/shopname": {"12": "The Cult's Stall"}
  },

  "scripts": {
    "sources": ["scripts/*.c"],
    "additions": [{"map": "m01", "programs": {"9001": "cult_start"}}]
  },
  "engine": {"sources": ["engine/*.cpp", "engine/include/**"]},
  "port": {
    "sources": ["port/*.c"],
    "api": "^1",
    "settings": {"render.scale": 2}
  },

  "settings": {
    "hard_mode": {"type": "bool", "default": false, "label": "Cultists hit twice as hard"}
  },

  "dev": {"start_at": "m01"}
}
```

### Metadata and identity

- **`format`** is required. A tool refuses a format it does not know and names the dc2tk
  version that reads it. Within one format every key is known: unknown keys and options are
  errors.
- **`id`** is the mod's identity. It is lowercase `author.name` (`[a-z0-9-]`, a dot between),
  and stays fixed for the mod's life. The folder or zip name means nothing. Dependencies,
  numbers, `#include "andrew.npc-kit/speech.h"` and saves all use the id.
- **`version`** is semver. **`tool`** is the dc2tk range the mod needs. **`game`** is the disc
  it is for (`dc2-pal` now, other regions later), since asset ids and data tables differ by
  region.
- **`targets`** says where the mod runs. The default, `["iso", "native"]`, is a promise of
  parity: the mod behaves the same on PCSX2 or a PS2 and on the native port, and the build
  refuses anything only one target can do. A mod that wants native-only features declares
  `["native"]` and may then use them (section 6).
- **`name`, `description`, `authors`, `license`, `homepage`** are for people. A mod manager
  can show them without building anything.

### Dependencies

- **`dependencies`** maps ids to semver ranges. They are pulled in and applied first.
- **`after`** only orders mods that are chosen anyway.
- **`conflicts`** refuses to build with a mod in a version range, so an author can say so
  before players find out.
- Ordering is a stable topological sort, as today. Every clash check from format 1 stays
  (same id, same place, same data value, same engine function or header, same whole file),
  and so does naming both mods in the error.

### Assets, files and data

- **`replace`** and **`add`** keep format 1's meaning, ids, options and patterns, with the
  options typed and documented in one place. Replacing an id the disc lacks is an error, and
  so is adding one it has; that is why there are two maps.
- **`files`** takes the raw disc-path lines that were `file:` keys, so `replace` and `add`
  hold only ids. A path starting `mod:` (`"mod:gill.chr"`) is a new file in the mod's own
  folder on the disc (`mods/<id>/gill.chr`), placed and named by the build. Scripts ask for it
  as `MOD_FILE("gill.chr")`, which removes the `mods/calculator/` convention.
- **New assets get ids.** `model:gill` and `texture:...` under `add` make new assets in the
  mod's namespace (built like `file:` with `like` today). Another mod reaches them as
  `model:andrew.spinning-cult/gill`.
- **`data`** holds table edits inline as the record lists `data/*.json` holds today, keyed as
  today. A string instead is a path to such a file, for big tables. `text` does the same for
  message edits by message number, with files for whole languages.
- **`disc/`, `raw/` and `files/`** go away. Raw files are `files` lines, and whole script copies
  become patches (`mod upgrade` runs `mod slim`).

### Scripts

`scripts.sources` lists the patch and library files. They keep `#pragma target` and every
directive. `additions` moves under `scripts`. `tests.c` is found under `scripts` and runs with
`mod test` as now.

### Numbers

A mod never chooses a save flag, counter, map program or monster variable. It names them, as
now (`NEW_FLAG(met_cult)`, `NEW_COUNTER(chants)`, `NEW_PROGRAM(show)`, `NEW_MONSTER_VAR(rage)`),
and the names belong to the mod's id, so two mods can both say `met_cult`. The build gives
every name a real number:

- **One allocator per mod set** (section 5). It assigns numbers for every mod in the set, so
  two mods in one set can never share a number. There is nothing to renumber by hand.
- **Assignments are kept** in the set's `set.lock`, so a save made with a set means the same
  after the set changes. Adding a mod takes only free numbers. A removed mod's numbers stay
  reserved, so the old saves still read correctly.
- **The only limit is the save itself.** The PS2 save has about 1,000 flags free for mods, and
  a set that needs more is refused with each mod's count. On the native target the same
  names can later go to a named store in the save, which has no such limit and needs no change
  to any mod.
- The ranges that avoid the disc's, the engine's and other code's numbers stay as they are
  today.

### mod.lock

This is JSON written only by dc2tk, shipped in the zip, and never re-sorted into `mod.json`:

- the decompiler hash of each script a patch was made against, for the stale-patch warning;
- the resolved dependency versions of the last build.

### Settings

`settings` declares options a player can change without editing the mod: `bool`, `int` with
`min` and `max`, `enum`, and `string`, each with a default and a label. A mod set (section 5)
gives the values. Scripts see them as constants when they are compiled
(`#if SETTING(hard_mode)`), data and asset lines can be switched by one
(`"when": "hard_mode"`), and port code reads them at run time.

### dev

`dev` holds what helps the author and must never reach players: `start_at` for now. `mod pack`
drops it, and only `build --dev` or `mod watch` reads it.

## 5. Mod sets and what a build makes

A **mod set** is what a player chooses: which mods, their settings, and the disc.

```json
{"format": 2, "game": "dc2-pal",
 "mods": [{"id": "andrew.spinning-cult", "version": "1.2.0", "settings": {"hard_mode": true}},
          {"id": "andrew.hd-ui"}]}
```

Beside the set file, dc2tk keeps `set.lock`: the numbers given to every mod's named flags,
counters, programs and monster variables (section 4), and the versions the set last built with.
It travels with the set and the player's saves, never with a mod.

One build of a set makes everything every target needs, under `build/sets/<hash>/`:

- **ISO target.** The image as today (`build -o modded.iso`). A set that holds a mod declaring
  `"targets": ["native"]` cannot build an image, and the error names those mods.
- **Native target.** The built game files as an overlay the port reads at start (today's
  override folder, made a normal feature instead of a debug one). Plus a port binary when the
  set has engine or port code; otherwise the stock binary is used. Scripts, assets and data
  then reach a native player through the same build as the ISO, which ends the drift in
  section 1.
- **`mod watch`** rebuilds the overlay and reloads, as now. With port or engine code it
  relinks and restarts the binary instead of only printing a note.

A player with the native release runs `dc2tk play set.json dc2.iso` (or a launcher does the
same), so nobody builds an ISO who doesn't want one. Building a set needs the disc but not the
decompiled tree: the build decompiles only the scripts the set's patches target, into a cache.
Engine and port code need clang for the native target. Only engine code for the ISO needs
Docker and MWCC.

## 6. Extending the port

Parity comes first: a mod with the default `targets` must work the same on both targets, so
everything in this section is refused for it. A mod opts out with `"targets": ["native"]`, and can
then use port code, port settings and port-only asset kinds. Everything below is for those mods.

A mod's `port` section adds C or C++ that is compiled into the port binary of the sets that
use it, against a versioned header, `port/include/port_mod.h`. `port.api` is the range of that
API the mod needs, and a build refuses a mismatch. The API grows from the hooks that
`port_debug.h` has now:

- **Lifecycle and frame:** start, each field, frame enter, drawn and exit, present, quit.
- **Input:** read and rewrite the pad state before the engine sees it.
- **Rendering:** the render settings (scale, MSAA, filtering, aspect), a pass after the frame
  is drawn, and texture lookups (below).
- **Files:** answer a file read before the disc (what the overlay does) and add files.
- **Commands:** register commands on the debug API and items in the debug window, so the
  debug tooling's scenario, capture and replay modules become ordinary extensions.
- **Settings and storage:** read the mod's settings, and keep named values in a section of the
  save the PS2 format lacks.

With that API:

- **The debug build becomes a mod** (`dc2tk.debug-tools`), built in the same way as any other.
  `port.py --debug` stays as a shortcut for "the stock set plus that mod", and the shipping
  binary has no debug code in it at all, which also settles PLAN fix 9.
- **4K and other render options are a data-only native mod:** `"targets": ["native"]` and
  `"port": {"settings": {"render.scale": 4, "render.msaa": 4}}`, no code.
- **Texture packs at higher resolution** are a port-only asset kind, `hd:texture:<id>`, under
  `replace`. They point at PNGs of any size that the renderer uses in place of the game's
  picture, matched by the picture's hash when it is uploaded. Nothing is built into the disc
  files, so they cost nothing on the ISO target and never break the game's memory limits.
- **Engine code still goes through `engine`.** It is shared by both targets, so a mod with
  the default `targets` keeps it within MWCC's C++. A native-only mod may use what clang
  allows.

A mod with `engine` or `port` code runs native code on the player's machine; a script mod runs
inside the game's script VM. `mod list` and `mod install` say which a mod is. Zips ship source
only. Prebuilt per-platform libraries (`port/bin/<os>-<arch>/`) can be added later as their own
key, once the port API is stable enough to keep a binary interface.

## 7. Staying compatible with the future

- **`format` changes only when the meaning of something already there changes.** New keys, id
  kinds, options and port hooks are additions within a format, and `tool` says which dc2tk a
  mod needs for them. An older tool refuses a newer key it doesn't know, naming the version
  that knows it, instead of ignoring it.
- **Keys are namespaced by what they change** (`replace`, `files`, `data`, `text`, `scripts`,
  `engine`, `port`, `settings`). A new layer is a new top-level key.
- **Ids carry their kind** (`texture:`, `model:`, `hd:texture:`, `data:items`), so a new kind
  of content never changes an old one. Mod-made assets are namespaced by mod id.
- **The port API is versioned on its own** (`port.api`), so the port can change its internals
  freely as long as `port_mod.h` keeps its contract.
- **The schema is written down** as `definitions/mod.schema.json`, generated from the Rust types
  that read `mod.json` (serde with `deny_unknown_fields`). Editors can then complete and check a
  `mod.json`, and the docs table cannot drift from the code.

## 8. Commands

| Command | Does |
| --- | --- |
| `mod new ID` | makes the folder with `mod.json` (format 2, the id, `game`, `tool`) |
| `mod asset`, `mod override` | as now, writing lines into the new keys |
| `mod check` | reads and validates `mod.json` and `mod.lock`, then everything `check` does now |
| `mod pack MOD` | writes `<id>-<version>.dc2mod` deterministically after the checks in section 3 |
| `mod install FILE` | copies a `.dc2mod` into the mods folder and reports its id, version, kind and dependencies |
| `mod upgrade MOD` | converts a format 1 folder: an id from the folder name, `file:` lines into `files`, data files inline, `disc/` and `raw/` into `files`, `files/` slimmed into patches, `start_at` into `dev` |
| `set upgrade MODS` | turns a format 1 mods folder into a mod set, moving `mods/numbers.json` into its `set.lock` so existing saves keep their flags |
| `build SET` | builds a mod set for both targets (`-o x.iso` for an image) |
| `play SET DISC` | builds the set for the native target if needed and runs it |

## 9. Order of work

1. **Done.** Typed `mod.json` (serde, `deny_unknown_fields`) and the schema; `format`, `id`,
   `version`, `game`, `tool`, `targets`, `dependencies`, `conflicts`, `dev`; `mod.lock`;
   `mod upgrade`. Names stay in `mods/numbers.json`, keyed by id instead of folder, until step 4.
   The rest of the build stays as it is. How it was built:
   - `dc2tk-script`'s `manifest` holds the types, `mod.lock` (the whole files a mod copied, which
     `mod.json` held as `files` and `includes`), `mod upgrade` and the index of mods by id;
     `semver` is a small range parser of its own (`*`, `^`, `~`, `=`, comparisons joined by commas),
     since the workspace has no semver crate.
   - The schema, `definitions/mod.schema.json`, is written by hand: there is no schemars here. A
     unit test checks its keys against the types. `"$schema"` is accepted in `mod.json` so editors
     find it.
   - The id is the mod's name everywhere the folder name was: dependencies, numbers, clash and
     build messages, `mod list`, generated wrapper names. `--mod` and the other commands take an id
     or a folder name. A patch includes another mod's file as `#include "<id>/x.h"` (the folder
     name still works too).
   - `build --dev` replaces `--no-start-at`: without it a build is a normal New Game. `mod watch`
     never used `start_at` and still doesn't.
   - `targets` is checked: an image (`-o`, `--dry-run`) refuses mods without `iso`, `--native` and
     `mod watch` refuse mods without `native`.
   - `mod new`, `mod save`, `mod override` and `mod asset` start a missing `mod.json`, with the id
     `local.<folder>` (`mod new` and `mod save` take `--id` for another).
   - The golden snapshot builds every disc image identically; only the logs name mods by id.
2. `files` with the `mod:` namespace, inline `data` and `text`, `scripts.sources`; drop `disc/`,
   `raw/` and `files/`.
3. Zips: reading a mod from a `.dc2mod`, `mod pack`, `mod install`.
4. Mod sets with `set.lock` (numbers move there; `set upgrade`) and the native target's overlay
   as a normal feature; `play`.
5. `port_mod.h` and the `port` section, with the debug build moved into `dc2tk.debug-tools` as
   its first user; render settings as data.
6. `settings`, and `hd:texture:`.

## 10. Decisions

- **Shared mods hold source only.** Players of mods with engine or port code need clang.
  Prebuilt per-platform libraries can come later as their own key, once the port API is stable.
- **Mods name their numbers; the build assigns them.** No mod picks a flag, counter, program or
  monster variable. Numbers are given per mod set and kept in its `set.lock` (section 4).
- **Patches may hold whole decompiled functions**, as `mod override` writes them. Whole script
  files are still not shipped, because they cannot be combined with other mods' patches.
- **Parity by default.** Mods run the same on the ISO and the native port unless they declare
  `"targets": ["native"]`, which unlocks port code, port settings and port-only assets and
  keeps the mod out of ISO builds.
