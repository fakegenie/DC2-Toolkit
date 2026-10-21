# Design: Rust, an asset tree, and mods by name

Three changes, in this order: decide the formats below, move the tools to
one Rust program, then build the asset tree and the new mod format on it.

All three are done: `dc2tk` is the only tool. (It began as a port of an
earlier Python tool, which it has replaced.)

## 1. The asset tree

`extract` already writes every file of the disc, and `decompile` turns the
scripts into a tree of C. The asset tree does the same for everything else:
`extracted/assets/` beside `extracted/scripts/`, each asset once, in a
format common tools open.

```
dc2tk assets extracted/disc    (or dc2tk assets pal.iso; writes extracted/assets/; --force replaces a tree already there)
```

```
extracted/assets/
  manifest.json                 every asset: id, kind, hashes, every place the disc has it
  textures/<group>/<id>.png     one per distinct picture
  models/<group>/<id>/          one per distinct character pack
    <id>.gltf, <id>.bin         skeleton, skin, meshes, motions as animations
  sounds/<bank>/<NNN>_<NAME>.wav  one per sound that plays a sample (<NNN>.wav when unnamed)
  text/<language>/<name>.txt    the message files' text, one per name and language (assets/text.md)
  music/<bank's folder>/<id>.mid  one per distinct sequence (music, ambient loops; assets/music.md)
  music/<bank's folder>/<bank>.sf2  its bank's instruments, to hear it with
```

Folders are only for browsing (`<group>` is the folder of the disc file
where the asset is first found: `chara`, `dungeon/monster`, `map/s/s51`);
nothing depends on them. What identifies an asset is its **id**. The whole
PAL disc takes 12-16 s (16 cores) and gives 750 MB in 15,646 files; the same
disc always gives the same tree, byte for byte, whether read from the image
or from `extracted/disc/`. Maps are not in the tree: `dc2tk map-export`
writes a map as a glTF scene of its placed pieces (`extracted/maps/<map>/`,
or every map with `--all`; [editing maps](../../docs/guide/maps.md)), and
the data tables are copied into a mod by `mod asset` ([data](../../docs/guide/data.md)).

### What is an asset

The tree reads four kinds of game file wherever they are, on their own or
as members of packs (as deep as `extract` goes): texture files (`.img`),
character packs (`.chr`), sound banks (`.snd`) and message files (`.mes`).
Over the PAL disc:

| | places on the disc | distinct (name and bytes) | distinct (bytes alone) | assets |
|---|---|---|---|---|
| pictures | 15,004 in 5,217 texture files | 7,036 | 6,452 | 7,036 textures |
| character packs | 2,997 | 2,744 | 2,692 | 2,410 models |
| sound banks | 861 | 846 | 846 | 846 banks, 16,107 sound entries |
| message files | 310 | 118 | 111 | 120 texts |
| sequences (`.sq` in banks) | 447 | 437 | 278 | 437 music |

An asset is a distinct content under one of the game's names, extracted
once; changing it changes every place the disc has it. A content found
under two names (a pack copied as `c02_win.chr`) is two assets, since the
game asks for each by its name; that is why there are more assets than
distinct bytes. A picture is its TIM2 bytes and whether its texture file
stores it swizzled; a pack or bank is its bytes.

- 334 distinct packs are not models: 309 hold no model (motions only, or
  pictures only: their pictures are textures all the same), 24 have no
  cfg (packs of other packs), one is not a pack. Most monsters' packs give
  their model in a cfg named after the pack (`b04a.cfg` in `b04a.chr`)
  instead of `info.cfg`; the tree reads such a cfg as the pack's
  `info.cfg`, so they are models.
- 6 texture files hold only render targets (no pixels) and give no texture.
- These are listed in the manifest's `unconverted`, with the reason, so
  `files` lines can still reach them.
- A member a pack holds twice under one name is reached by the first, as a
  build finds members by name; the second (one texture file on the PAL
  disc) is left out. A path `DATA.HD4` lists twice is read once, the first.
- The meshes of maps (`.mds` pieces in `.pcp`/`.mpk` packs) are not assets
  of their own: they belong to the maps, which come later.

### Ids

An id is the kind and the game's own name for the asset: `texture:c02a02`,
`model:c02fuku1`, `bank:EV_000_130`. Pictures are named as their texture
file names them (cp932 where the bytes are cp932: `texture:森の美女`),
packs and banks by their file name without the suffix.

Names mostly identify content, but not always (793 picture names have more
than one version, such as a map's pictures redrawn for another map). The
version that is a file of its own on the disc (`snd2/sp/SP_003.snd`) keeps
the bare name before copies inside packs (`menu/0/inv0_bg.pac`'s), and
otherwise the version found first does. The others take `@` and where they
are first found: the name of the disc file holding them when they are a
member (`texture:a01ia_01@a01ic` for `map/a/a01/a01ic.ipk`), else the disc
file's folder with `-` between parts (`model:c02_base@dungeon-chara`); if
that is taken too, `.2`, `.3`, ... Ids differ even ignoring case, as file
names on Windows must. The same disc always gives the same ids.

Sounds are named by the names the definition set's header
(`definitions/pal/include/pal.h`, which `disc.h` sits beside) gives the
entries of a bank: `enum se_ev_000_130_pal` names those of `EV_000_130`,
and the system, base and battle enums those of the bank their comment
names (`snd2/SY_000`, `snd2/BS_999`, `snd2/FG_000`). They name the bank's
bare-named version. So `sound:SE_EV_000_130_GRIFFON_BREAK_WINDOW`, and
`sound:EV_000_130/4` is the same entry by number (the manifest's `aliases`
maps one to the other); entries without a name are only by number
(`sound:EV_000_130/2`, `sound:SP_001@inv0_bg/3`). 1,497 of the 16,107
entries have names.

Texts are `text:`, the language and the file's name without its language
number (`text:en/allmenu`, `text:fr/system` for `meswin/system_2.mes`): one
id for every version of a file of that name and language, unlike the other
kinds, since a mod's text changes messages and each place keeps the rest
of its own version. Copies in the old language folders take `@` and their
folder (`text:en/system@meswin-usa`). See [message files](../../docs/guide/text.md).

### The manifest

`manifest.json` lists, for every id, the kind, a SHA-256 of each file as
extracted, a SHA-256 of the game bytes, and every place: the disc path and
member chain, plus the picture (its entry in the texture file) or the
sound entry within it. A mod build reads it to find the places to change;
nobody edits it.

```json
{
  "counts": {"texture": 7036, "model": 2410, "bank": 846, "sound": 16107, "text": 120, "music": 437},
  "assets": {
    "texture:c02a02": {
      "kind": "texture", "name": "c02a02",
      "files": {"textures/chara/c02a02.png": "e081…"},
      "game_hash": "dd2e…", "format": "PSMT8", "width": 128, "height": 128,
      "places": [
        {"path": "chara/c02a.chr", "members": ["c02a.img"], "picture": 1},
        {"path": "event/LM/c02ev.chr", "members": ["c02a.img"], "picture": 2}
      ]
    },
    "model:c02_base": {
      "kind": "model", "name": "c02_base",
      "files": {"models/chara/c02_base/c02_base.gltf": "21b2…", "models/chara/c02_base/c02_base.bin": "cf1a…"},
      "game_hash": "8843…", "textures": ["texture:c02a01", "texture:c02a03"], "placeholders": [],
      "places": [{"path": "chara/c02_base.chr", "members": []}]
    },
    "bank:SY_000": {
      "kind": "bank", "name": "SY_000", "folder": "sounds/SY_000", "game_hash": "…",
      "sounds": ["sound:SE_CURSOR_MOVE", "…"], "places": [{"path": "snd2/SY_000.snd", "members": []}]
    },
    "sound:SE_CURSOR_MOVE": {
      "kind": "sound", "bank": "bank:SY_000", "entry": 0, "plays": "sample",
      "label": "カーソル移動", "category": "カーソル移動",
      "files": {"sounds/SY_000/000_CURSOR_MOVE.wav": "bfaf…"}, "game_hash": "9b72…",
      "places": [{"path": "snd2/SY_000.snd", "members": [], "entry": 0}]
    }
  },
  "aliases": {"sound:SY_000/0": "sound:SE_CURSOR_MOVE"},
  "unconverted": [{"path": "chara/c01_fishing.chr", "members": [], "error": "the pack's info.cfg names no model"}]
}
```

(Abridged and folded onto fewer lines here; the file is serde_json's
pretty form, a member to a line, two spaces an indent.) A text asset also
has its `language` and how many `versions` of the file its places hold; a
music asset its MIDI and SoundFont `files`. Assets are sorted by id. A
`files` hash tells a build whether a file in a mod is still the extracted
one. The shape is `dc2tk-assets`' `manifest.rs`, which the tree writes and
the index of ids and `assets-check` read back.

### Formats

- Textures: PNG, indexed when the picture has a palette (as `mod asset --as
  png` writes now).
- Models: glTF 2.0, as `--as gltf` writes now, with the buffer renamed to
  `<id>.bin`. Images point into `textures/` by relative path, so Blender
  finds them, and are not copied: the pack's own pictures at their texture
  ids, a texture the pack does not have at the bare-named texture of that
  name, and a texture the disc has nowhere (18 on the PAL disc) stays the
  export's placeholder, written beside the model under `textures/`. A
  model built from the tree takes its meshes, skin and pose from the glTF;
  its textures are assets of their own and change through their texture
  ids.
- Sounds: WAV per entry that plays a sample (3,771). The other entries
  are listed in the manifest with what they play, but have no WAV: the
  ones that play a sequence (`plays` `seseq`, 2,142, a `.mid` member of
  the bank, and `sq`, 437, whose `.sq` sequences are the music assets
  below), and entries that play nothing (9,757 numbered lines of the
  banks' cfgs). A bank's folder of WAVs is what `mod asset --as wav`
  would write, with names after the numbers, less the entries of a
  bank's other version that are the first version's sounds: their WAV
  is in that version's folder only.
- Text: UTF-8, a message after each `@id` line, one file per name and
  language whatever versions the disc has (see [message files](../../docs/guide/text.md)).
- Music: a standard MIDI file per distinct sequence, and beside it a
  SoundFont of its bank's programs to hear it with, which is never read
  back ([music](../../docs/guide/music.md)).
- Maps: not in the tree (above). The `.map` file is text placing `.mds`
  pieces (position, rotation, scale), so a map is a glTF scene of placed
  meshes; a mod's `map:` line builds an edited scene back ([editing maps](../../docs/guide/maps.md)).

Every exported file builds back to the disc's own bytes when unedited, at
every place the manifest lists: the hidden `dc2tk assets-check SOURCE ASSETS`
(SOURCE the image or an extracted disc)
checks every file's hash, builds each texture into each texture file the
manifest gives for it, each model's glTF and buffer into each of its packs,
each bank's folder into each of its places, each text into each of its
message files and each music asset's MIDI file into each of its sequences,
compares with the disc, and checks every texture file, pack,
bank and message file of the disc is listed. On the PAL disc: 15,764 files,
15,004 picture places, 2,663 pack places, 861 bank places and 310 message
file places build back, no problems.

## 2. Mods by name

A mod is a folder with a `mod.json`, read into typed records that refuse
an unknown key (`dc2tk-script`'s `manifest`): its format (2), its id
(`author.name`, which other mods, numbers and messages use instead of the
folder's name), its version, the mods it depends on, follows or conflicts
with (semver ranges), the targets it runs on, and its `dev` settings.
Where files sit inside the folder does not matter: `mod.json` says what
each one replaces, by asset id (section 1). Built in `dc2tk` (`dc2tk-script`'s `replace`, the index of ids
in `dc2tk-assets`' `ids`).

```json
{
  "format": 2,
  "id": "andrew.castle-showcase",
  "version": "1.0.0",
  "name": "Castle showcase",
  "authors": ["Andrew"],
  "description": "Monica's clothes reshaped, a green soldier, new event sounds.",
  "dev": {"start_at": "s51:500"},

  "replace": {
    "texture:e254a1": "art/soldier_green.png",
    "model:c02fuku*": "models/monica/c02b.gltf",
    "sound:SE_EV_000_130_GRIFFON_BREAK_WINDOW": "sounds/boom.wav",
    "sound:SE_EV_000_130_MONICA_SWING_SWORD": { "file": "sounds/chime.wav", "volume": 90 },
    "bank:SP_003": "sounds/SP_003",
    "texture:a01ia_01@a01ic": { "file": "art/floor.png", "only": ["map/a/a01"] }
  },
  "add": {
    "sound:SE_MOD_BEEP": { "file": "sounds/beep.wav", "bank": "SP_004" }
  },
  "files": {
    "snd2/event/EV_000_130.snd": "raw/bank.snd",
    "mod:ui.img": { "file": "ui", "like": "texture:logo_d00" },
    "mod:calc.chr": { "file": "model/calc.gltf", "like": "model:uki" }
  }
}
```

- **Keys are asset ids** as the asset tree gives them, the kind's prefix
  and the name ignoring case, and `sound:BANK/N` for a named entry too.
  Values are a file in the mod, by its path from the mod's folder (`/`,
  nothing outside it), or an object with `file` and options: `only`
  (replace), `volume` (sounds, 0 to 127; set even when the WAV is the
  bank's own) and `bank` (add). An unknown option, a file the mod does not
  have or of the wrong kind, is refused when `mod.json` is read. A
  `files` line also takes `like` (below).
- **What each kind takes.** A texture: a PNG, built into the picture at
  each place with the disc's picture as the template (palette, mip levels),
  as a texture folder's PNG is. A model: a `.gltf` file with its buffer
  beside it (Blender's "glTF Separate", or the tree's own
  `models/.../<id>.gltf`), whose images are not read: the model's
  textures change through their own ids. Or a folder as `mod asset` writes
  it, the export's `c02b.gltf`, buffer and `textures/` (PNGs edited there
  go into the pack, as the `--as gltf` folders under `raw/` do); a folder holding
  one other `.gltf` is that file. A sound: a WAV for that entry. A bank: a
  folder of WAVs named by entry number (`004.wav`, or `004_NAME.wav` as the
  tree names them), for entries the bank has; new entries are `add`'s. A
  music asset: a MIDI file (`.mid`), built into the sequence with the
  disc's as the template ([the MIDI file](../../docs/guide/music.md#midi-rules)).
- **Every place.** A replacement reaches every place the disc has the id,
  each built with that place's own file as the template. `only` keeps the
  places under the disc paths it lists: a place itself
  (`dungeon/monster/e254a.chr.d/e254a.img`), or a folder or pack holding
  it (`map/a/a01`, `map/a/a01/a01ic.ipk`), ignoring case, `*` and `?`
  allowed; one that keeps no place is refused.
- **Patterns** (`model:c02fuku*`, `*` and `?`, ignoring case, over ids and
  aliases of that kind) replace every id they match with one file, such as
  every costume pack of Monica (one glTF fits them all: each place is built
  from it with its own pack as the template). An id given exactly wins
  over a pattern; two patterns giving one id are refused, as is a pattern
  matching nothing.
- **`files`** replaces a disc file or pack member by path, as `extract`
  lays the disc out (`a.pac.d/b.chr.d/c.img`): the escape hatch for
  anything without an id (the 340 game files of the manifest's
  `unconverted`, scripts' packs, anything else). A file is the game file
  itself, a folder converts by the game file's suffix (PNGs for a `.img`,
  WAVs for a `.snd`, a glTF folder for a `.chr`) whatever it is named, and
  a line with `like` (or a `mod:NAME` path, the mod's own
  `mods/<id>/NAME`) adds a new file.
- **`add`** appends a sound entry to every place of a bank, numbered on
  from the bank's last entry (in build order, when several mods add to one
  bank). Its name is a constant in the mod's patches, as `NEW_FLAG` names
  are (`play_se(SE_MOD_BEEP)`); `build` prints the number. `mod check`
  without a disc compiles with 0 for it. Other new assets (a model or
  picture under a new name) need a script or model to ask for that name,
  so they are `files` lines, by the path the script loads (`mod:` ones as
  `MOD_DIR "NAME"`); the design's
  "mod's generated header" is not needed while patches see the names.
  A `music:BG_NNN` line under `add` (`{"file": "x.mid", "like": "BG_193"}`)
  makes a new music file `snd2/bgm/BG_NNN.snd`, the like bank playing the
  MIDI file, for `load_bgm(NNN)`; the number is the mod's choice, one the
  disc lacks ([music](../../docs/guide/music.md#add-a-new-music-file)).
- **New files from common formats.** A `files` line with `like` makes its
  game file whole, from PNGs or a glTF, with the disc's file `like` names
  as the template, so a mod can have a picture file or model pack of its
  own (at a `mod:` path or one the disc lacks; on a path the disc has it
  replaces the file whole). `like` is an asset id (`texture:logo_d00`, the
  texture file holding that picture; `model:uki`, that model's pack, at
  its first place) or a disc path (`dungeon/sphida/sphida_bar.img`,
  `chara/uki.chr`, members as `a.chr.d/b.img`), of the same kind as the
  path: a texture file for `.img`, a character pack for `.chr`.
  - A **texture file** (`.img`) takes a PNG, or a folder of PNGs: one
    picture each, named by its file name without `.png` (1 to 31 Latin-1
    characters, distinct ignoring case), the name `set_image_texture` and
    models' materials ask for. Each is written in the format of the
    template's picture (the one the id names, or the file's first): its
    pixel format, palette size, swizzling, mip levels and header fields,
    with the PNG's own size. The game's 2D pictures, and prop textures,
    are 8-bit indexed (PSMT8) with a 256-colour palette with alpha, stored
    swizzled; a PNG with at most 256 colours keeps them exactly, any other
    is quantised to 256 (median cut). Alpha is the PS2's: PNG 255 is 0x80,
    opaque, in 129 steps. Sides are 1 to 1024 texels (TEX0's limit), a
    multiple of 16 wide for swizzled 8-bit pictures; the game's own 2D
    pictures are not all powers of two (`logo_d00` is 384x128), as event
    images are drawn by texel rectangles. The file is an IM3, as every
    texture file of the game's is, its pictures in file-name order.
  - A **character pack** (`.chr`) takes a glTF (`.gltf` with its buffer
    and images beside it, a `.glb`, or a folder holding one), and is named
    after its path: `calc.chr` holds `calc.img`, `calc.mds` and `info.cfg`.
    The model is the glTF alone: every node a frame, with its translation,
    rotation and scale under its parent (several roots go under a frame
    named after the pack), and each mesh node's triangles (positions,
    normals, the first UVs and colours) its frame's mesh. A material's
    base colour texture is a picture of the pack's texture file, named as
    the image is (its `name`, else its file name), in the format of the
    template's first picture, with sides that are powers of two (a model's
    UVs wrap at the power of two above a picture's size); PNG only. A
    material with no texture names none. The template gives the MDS
    version and its `info.cfg` (or the cfg named after it, for monsters'
    packs), which becomes the new pack's `info.cfg` (the name
    `load_chara` asks for): `NAME`, `IMG` and `MODEL` name the new
    members, and what
    belonged to the template's model alone (motion sets with their keys,
    sequences, sound cues, cloth, effects, `VERTEX_ANIME`, `SHAPE_ANIME`,
    shadow and LOD models, outlines and attach points on frames the new
    model lacks) is dropped; the rest (`MATERIAL_ANIME`, the whole-model
    `OUTLINE`, `BODY_SIZE`, `SCALE`, comments) stays. A skinned mesh is
    skinned as the game's are: the pack gains the `.wgt` and `.bbp`, and
    `VERTEX_ANIME` names it; the glTF's animations become the `.mot` and
    `MOTION 0`'s KEYs ([the model formats](assets/models.md) 7.4). A
    static prop's pack is the template to pick for a prop:
    `model:uki` (`chara/uki.chr`, the fishing float: one picture, one
    mesh, no motion), or `model:lure01` and `model:hari` like it; a
    villager's (`model:p01_01a`) for a character.
  - Exported again (`mod asset ISO NAME PATH --as png`, `--as gltf`), the
    file gives back its pictures, and its meshes and pictures.
- **Other kinds.** A `text:` id takes a text file as the tree writes it,
  and changes at every place the messages whose text differs from the
  exported one ([message files](../../docs/guide/text.md)); a `music:` id a MIDI file
  (above). `"map:s17": "maps/s17/s17.gltf"` builds an edited map scene
  back into the map's files, and under `add` makes a new map from a map's
  scene (`"like": "m05"`, with `"script"` for an event script of its own;
  [editing maps](../../docs/guide/maps.md)). `"data": {"items":
  "data/items.json"}` (and `shops`, `monsters` and the other tables `mod
  asset` lists) edits a table of game data as JSON, merged with other
  mods' tables ([data](../../docs/guide/data.md)).
- **Scripts** stay as they were: `.c` patches with `#pragma target`,
  anywhere in the folder (or where `"scripts": {"sources"}` says);
  `"scripts": {"additions"}` adds to scripts the mod does not copy, and
  `"scripts": {"whole"}` lists whole-file copies. The mod's `id` names it,
  never its folder; `name` and `version` are shown by `mod list`, and
  `authors`, `description` and the rest are for people
  ([mod format 2](mod-format.md)).

### How a build finds the places

`build` makes the index of ids (`ids::Index::from_disc`) from the disc it
builds from, its image or an extracted disc, only when a chosen mod names
an id: the same walk and naming as `dc2tk assets` (`tree::named`, shared),
without converting anything. On the PAL disc it takes 0.6 s and gives the
asset tree's manifest exactly (all 26,399 texture, model, bank and sound
ids, 1,497 aliases and every place, compared; the text and music ids are
indexed the same way). It does not read
`extracted/assets/manifest.json`: a tree can be stale, made from another disc or
with other definitions, and a build should not need one. The definition
sets (`--defs`) name the sound entries, as for `dc2tk assets`. (The index
counts as models the packs whose cfg names a model; the tree, the ones its
export converts: on the PAL disc the same.)

Then, on top of the scripts and the `files` lines' files, it builds
models, then banks (every entry a mod changes at one place in one go),
then pictures (every picture of one texture file in one go), outer places
first, so a texture goes into a pack another line's model rebuilt; then
texts and music. New music files, the `map:` lines and the data tables
follow, each step over the files the steps before wrote. Each
changed place is read back from the new image, as `files` lines' files are, and
printed with its ids:

```
mod castle-showcase: 1 patches, 0 files, 0 game files
  replaces texture:e254a1 at 1 place
  replaces model:c02fuku1 (by model:c02fuku*) at 1 place
  ...
replaced: dungeon/monster/e254a.chr.d/e254a.img (texture:e254a1, texture:e254a2; mod castle-showcase, 35088 bytes, was 35088)
```

### Clashes

Refused at build (and by `mod check --disc`), naming the mods:

- one id replaced by two mods;
- one picture, sound entry or model place changed by two mods through
  different ids (a bank's folder in one and a sound's id in another);
- a change inside, or around, a game file another mod replaces whole by
  a `files` line.

A model in one mod and pictures of its pack in another go together: the
model is built first and the pictures go into the built pack. (The design
refused all overlapping places; that would have kept a recolour of Monica
and a reshaping of her apart, since her pictures live inside her packs.)
Within one mod, an entry given twice (by its bank's folder and its id) is
refused. `mod list`, which has no disc, shows each mod's lines and the ids
two mods both name.

### Commands

ISO here, and `--disc`, is the disc image or an extracted disc
(`dc2tk_disc::DiscSource` reads both alike).

- `mod asset ISO NAME ID...` copies assets off the disc by id or pattern,
  each in its common format (`textures/<name>.png`, `models/<name>/` as
  the export writes it, `sounds/<bank>/<NNN_NAME>.wav`, `sounds/<bank>/`)
  and adds its `replace` line; an id the mod has a line for is left alone.
  Paths (`chara/c02a.chr.d/c02a.img --as png`) copy into `raw/`, each
  with its `files` line.
  It reads the disc, not `extracted/assets/`: the files are the same as the tree's,
  and a mod should not need a tree.
- `mod check SOURCE NAME... --disc ISO` resolves every line, shows what
  each reaches and refuses clashes; without `--disc` it checks the lines
  and files only.
- `mod list` shows each mod's lines (`replaces texture:e254a1: ...`).

## 3. One Rust tool

One program, `dc2tk`, does everything: what was built is described in
[src/README.md](../README.md). A Cargo workspace (`src/Cargo.toml`, the
crates in `src/crates/`):

| crate | what it holds |
|---|---|
| `dc2tk-disc` | the ISO image, the file index and packs, extract and pack, reading an image or an extracted disc alike (`DiscSource`), writing a disc, the text files the tools write |
| `dc2tk-assets` | textures, sound banks, music, models and motions, maps, message files, the data tables, the asset tree |
| `dc2tk-script` | stb, the VM, assembly, decompile, parse, compile, the SDK, definitions, names, mods, and the stages `build` runs |
| `dc2tk` | the command line: arguments, the library's stages, what they did |

Each crate uses only the ones above it in the table; how they are laid out
inside is in [internals](internals.md). The binary is thin on purpose:
decompiling a whole disc, building a mod's maps and data tables and the
memory check are library functions, so the tests reach them as `build`
does. The definitions (`definitions/`), native tables (`definitions/natives/*.json`) and
SDK headers are data files. Most work is per file (decompile 1,398 scripts,
convert 5,217 texture files), so it runs across all cores (rayon).
