# Definitions

A definition set is a directory of knowledge about a body of compiled
scripts: what its functions are called, what its numbers mean, where its
text lives. The decompiler reads one or more definition sets alongside the
binaries and writes source that uses that knowledge. A definition set
contains no game code and no game text, so it can be published and shared;
anyone with the binaries gets the same readable source.

`definitions/pal/` is the set for the PAL retail disc. The same format
serves other versions of the game (NTSC, Japanese), Dark Cloud 1 once its
engine is supported, and mods.

## Engine and content

Knowledge splits in two, and only the second kind lives in a definition set.

- **Engine knowledge** is true of every script the engine runs: the
  instruction set, the native tables and what each native does, engine
  enums such as motion flags, generic helpers. It lives in
  `definitions/natives/` (the native tables, generated from the engine's
  decompilation by `dc2tk natives`) and `sdk/` (the C headers scripts
  include). It sits beside the definition sets but is not one: `--defs`
  never layers it, and `--natives` names another folder of tables. A mod
  author writing new code uses the SDK and nothing else.
- **Content knowledge** is true of particular scripts: that this function
  is the baker's dialogue, that flag 520 means the bridge is repaired, which
  file holds a scene's dialogue. It lives in a definition set.

## How definitions find things

A definition set never stores the code it describes, so every entry names
its target by something that can be computed from the binary.

| Target | Identified by |
|---|---|
| Function | fingerprint of its bytecode, with a loose fingerprint as fallback |
| Script | its path on the disc; language variants share an entry |
| String | hash of the string |
| Global | script path and slot |
| Message | message id and the text file the script loads |
| Number | the kind of the place it appears (see value kinds) |
| Library | fingerprint of its member list |
| Line in a function | function fingerprint and instruction index |

**Function fingerprints** (`src/crates/dc2tk-script/src/fingerprint.rs`) are the foundation, so
their definition is fixed and versioned. The fingerprint is the first 16
hex digits of a SHA-1 over: the function's record (argument count, local
count, all ten argument types); its unused strings; its instructions, with
jump targets made relative to the function, each call target replaced by
the callee's fingerprint and each string push replaced by the string
itself; and the native table it runs against. Padding, the record's name
word and position in the file are excluded. Two functions with the same
fingerprint are the same code calling the same code, wherever they sit and
whatever the decompiler makes of them.

The hashed text is a compact JSON payload,
`{"version":1,"table":..,"arg":..,"local":..,"arg_types":[..],"unused":[..],"code":[[op,arg1,arg2],..]}`,
with jumps as `["jump", offset]`, calls as `["call", fingerprint]` and
string pushes as `["string", text]`. `fingerprint.rs` writes it by hand
rather than through a JSON library, so that no change to how the tools
write JSON elsewhere can change a fingerprint the sets store. Functions
are fingerprinted in file order, so a callee is named by its fingerprint
only when it comes earlier in the file; a call to the function itself or
to a later one keeps the callee's offset from the code section, as does a
string push the block does not hold.

The **loose fingerprint** is the same with string contents blanked and
unused strings dropped. The disc's French, German, Italian, Spanish and
Japanese scripts are mostly the English code with different strings, so
they share loose fingerprints, and a name given to one copy reaches all of
them. Lookups use the exact fingerprint first. Then a function takes the
entry of the function at the same position in a language variant of its
script (`X_3.stb` and `X_1.stb` list their functions in the same order),
when both have the same loose fingerprint. Last comes the loose fingerprint
on its own, used only when every entry with that loose fingerprint agrees on
the name: functions that differ only in their strings, such as one talk
handler per villager, share a loose fingerprint without being copies of
each other.

## Layout

```
definitions/natives/    the engine's native tables, one JSON file a table
                        (event, monster, action, effect, arg): each native's
                        number, engine name and stack accessors
definitions/pal/
  definitions.json      manifest
  names/                functions: names, parameters, locals, docs
  scripts/              per script: doc comment, global names
  strings/              translations of strings, per language
  include/              content headers included by every script
  notes/                research notes in Markdown
  libraries.json        library paths and docs
  disc.json             the hash of each script on the disc the set is for
  paths.json            readable names for disc folders and scripts the disc
                        does not name (dungeons, the main characters)
  sounds.json           the event sound bank each sound set loads (column 15
                        of snd2/MAP_index.txt), for naming play_se entries
```

Every part is optional. A new set can start with just a manifest. The
tools write these files as JSON with a two-space indent, text unescaped, LF
line endings and a final newline, and read CRLF the same.

### Manifest

```json
{
  "name": "pal",
  "description": "DC2 PAL (SCES_511.90) retail scripts",
  "engine": "dc2",
  "extends": [],
  "language": "en",
  "include": [],
  "tables": [
    {"prefix": "dungeon/monster/", "table": "monster"},
    {"prefix": "mainchr/", "table": "action"}
  ],
  "text": {"languages": {"ja": 0, "en": 1, "fr": 2, "de": 3, "it": 4, "es": 5}}
}
```

- `extends` lists definition sets this one builds on, as paths relative to
  this one. A mod's set extends `pal`, so it only adds what the mod
  changes. Sets are applied in order and later ones override earlier ones
  entry by entry. On the command line, `--defs` can be repeated to the same
  effect; without it the shipped `definitions/pal` applies, and `--no-defs`
  applies none.
- `language` picks the language for comments: translations and text read
  from the disc.
- `include` lists headers from the set's `include/` directory that every
  decompiled script includes; they are also on the include path when
  compiling.
- `tables` maps disc paths to native tables; the first prefix that matches
  wins and anything else uses `event`. This is content, not engine: it is
  the PAL disc's folder layout.
- `text.languages` maps language names to the digit the disc puts on
  localized files (`e215_1.txt` is English).
- `engine` names the engine the set is for. Only `dc2` exists today, and
  the tools do not read it yet (nor `name` and `description`).
- `executable` names the disc's boot executable (`"name": "SCES_511.90"`;
  `extract` saves it beside the archive) and where tables inside it live,
  such as the monster table
  (`"monster_table": {"address": 3415456, "record_size": 184}`); addresses
  differ between disc versions, so they belong to the set.
- `disc.json`, beside the manifest, holds the hash of each script on the
  disc the set was made from (`dc2tk defs disc`; the first 16 hex digits
  of the file's SHA-1, under `scripts` by disc path), so tools can tell
  whether a disc matches the set or has been modified.

### Names

`names/<table>.json` maps function fingerprints to what to call the
function and its variables:

```json
{
  "functions": {
    "9c1f2a7b3e4d5f60": {
      "name": "talk_to_baker",
      "loose": "41d7c09a2e6b3f15",
      "doc": "Baker dialogue; picks lines by quest progress.",
      "params": ["npc", "player"],
      "locals": ["active_chara", "quest_state"],
      "types": {"quest_state": "enum quest_state"},
      "returns": "void"
    }
  }
}
```

Every field is optional. `loose` is the loose fingerprint, so the entry
also names language variants. `params` and `locals` name variables in
declaration order. `doc` is printed as a comment above the function.

Several fingerprints may share a name: versions of the same helper should.
When two different functions with the same name meet in one file, the later
one gets a numbered suffix (`swing_attack_2`); an entry point in that
position is named for its program instead (`on_stopped`, `start` for
`MAIN`, `return_zero_for_program_212`). A file that holds the same code
twice or more (identical stubs, which no fingerprint can tell apart) names
the later copies by their use: for the one function that calls a copy
(`unlock_interrupts_for_recoil_from_hit`, a caller's `on_` left off), or
`unused_do_nothing` for a copy nothing calls or starts. Copies called from
several places keep a number.
Library functions keep one name everywhere, so where two libraries that
meet in some script both have a function of the same name, one is numbered
on every disc file. Names that clash with a native, helper, constant or
keyword get a suffix too, as do parameter and local names that repeat or
hide a function the body calls.

`types` gives variables a value kind or an enum (by the names in `params`
and `locals`, or their default names). The variable is declared with it
(`enum quest_state quest_state;`), and numbers assigned to it, compared
with it, and passed for a parameter of that kind print as the enum's
names. In source, declare a variable with a kind or enum and `learn`
records it; it compiles as the kind's base type. `returns` overrides the
inferred return type (`void` only where no return gives a value); `learn`
records a declared return type that differs from what the decompiler
infers.

`notes` attaches comments to lines: `{"4": "22 is the town theme."}` puts
`// note: 22 is the town theme.` above the statement that starts at
instruction 4 of the function. In source, write a comment starting
`note:` on its own line above a statement; `learn` records it with the
statement's instruction index. Other comments are the decompiler's own
(dialogue, glosses, trigger points) and are not learned.

### Scripts

`scripts/<disc path>.json`, one file per script, mirroring the disc:

```json
{
  "doc": "Sindain riverbank: Max meets Lin and learns to fish.",
  "globals": ["rod_given", "visits"]
}
```

`doc` is printed at the top of the file. `globals` names the script's
globals in slot order; a global that included library code also uses keeps
its slot name, so the library compiles in every script that includes it.
An entry also applies to the script's language variants (`X.stb`,
`X_1.stb` ... `X_5.stb`) that have no entry of their own.

### Strings

`strings/<language>.json` maps string hashes (`src/crates/dc2tk-script/src/notes.rs`,
`string_key`) to translations, shown as comments beside the code that uses
the string:

```c
set_actor_face(0, "笑顔", "まばたき");  // smile; blink
```

Only short asset names (motions, faces, effects) belong here. Dialogue is
read from the disc.

### Content headers

`include/*.h` are ordinary script headers, typically of constants: story
flags, items, characters, maps, sounds. They are included by every
decompiled script and used when compiling, so `set_flag(FLAG_BRIDGE_REPAIRED,
1)` round-trips. Engine constants (motion flags, pad buttons) belong in the
SDK instead. A name may be declared once across the SDK and every set's
headers: an enum member that repeats another constant's name, or a
`#define` with a second value, stops decompiling and compiling with an
error naming both, as clang would.

The PAL set ships `include/pal.h` with `enum flag_story`, a name and a doc
comment for each of the 186 save flags the retail scripts set or read by
number. Because `set_flag` and `get_flag` take a `flag`, and a kind takes
constants from every enum named after it, these print wherever a literal
flag number appears.

A decompiled tree includes these headers from the set itself, so they are
not learned: to rename a constant, rename it in the set with

```
dc2tk defs rename definitions/pal FLAG_S18_ARRIVAL_SEEN FLAG_YORDA_TREE_ARRIVAL_SEEN --in extracted/scripts
```

which renames the declaration in `include/`, the set's docs, line notes and
notes that name it, and the variables, parameters and globals the set names
after it (the decompiler calls what `get_flag(FLAG_X_SEEN)` reads `x_seen`,
and `learn` records such names, so a set would otherwise keep the old one).
Each `--in` folder, a decompiled tree or a mod, gets the same renames in
its source; a tree's files that held no edits stay unedited for `changes`
and `mod save`. A fresh decompile then prints the new name everywhere, and
every script compiles to the same bytes. The new name must not be declared
anywhere yet.

### Libraries

Libraries are found automatically and get a default home under `lib/`, in
the folder their users share (`lib/map/map_29.c`). `libraries.json` gives
them a better path and a doc comment, keyed by library fingerprint (a hash
of the native table and the member functions' fingerprints, in order):

```json
{
  "libraries": {
    "5e0c4f1d9a2b7c3e": {"path": "lib/town/talk_v29.c", "doc": "Shared town talk helpers."}
  }
}
```

The path must start with `lib/` and end with `.c`, and uses the game's codes
for folders (`lib/map/s/s51/`); the decompiler adds the same readable names
it gives the scripts' folders (`lib/map/s/s51_raybrandt_castle/`). Function
names inside still come from `names/`, and default function names keep using
the library's automatic name, so moving a library never changes them.

`globals` names the script globals a library's code uses, by their default
slot name:

```json
{"path": "lib/map/s/s05/scene_setup.c", "globals": {"g0": "max_chara_slot", "g1": "partner_chara_slot"}}
```

Library code is one file shared by every script that includes it, so each
of those scripts must declare the slot under the same name. A script's own
entry cannot rename a slot that included code uses; the library's name is
used instead. Where two libraries included by one script disagree about a
slot, or give two slots the same name, those names are dropped and the
slot keeps its default name everywhere the libraries are used. `learn`
records these names too: rename such a global in the library file and the
scripts that include it, and the library's entry takes the new name.

## Text from the disc

The decompiler comments every message call with the line it shows, read
from the user's disc at decompile time:

```c
play_voice_message(2300010);  // "Hey...isn't that Master Utan?"
```

It needs no per-script configuration. The SDK types the relevant native
parameters with value kinds (`message`, `message_file`, `directory`); the
decompiler follows `change_dir` and `load_message_file` through each
script, localizes each file name to the set's language, and looks message
ids up in those files. A script function that passes one of its parameters
on to a message parameter gets a message parameter too, so calls to helpers
such as `play_voice_message` are commented as well. Library files are
commented where every script that includes them agrees on the text.

The file formats and the language rules are in
[definitions/pal/notes/messages.md](pal/notes/messages.md).

## Languages

Comments can be in any of the disc's languages, chosen by the manifest's
`language`.

- Dialogue by message id is read from the disc in that language.
- Short asset names use `strings/<language>.json`.
- Inline dialogue in the French, German, Italian and Spanish scripts lines
  up string for string with the English script (`X_3.stb` with
  `X_1.stb`), so each line gets its sibling in the chosen language as a
  comment, read from the disc. Library files get it where every script that
  includes them agrees.
- The original Japanese scripts do not line up with any English script, so
  their inline dialogue has no English on the disc. The shared set leaves
  those lines untranslated: shipping our own translation would publish the
  game's dialogue. A user can add their own translations in a personal set
  that extends `pal`.

## Value kinds

Numbers and strings mean different things in different places. The SDK
declares kinds with `typedef` and types native parameters with them:

```c
typedef any message;
typedef string message_file;

extern void show_message(int mes, message text, OPTIONAL(int *char_count)) NATIVE(192);
```

A kind reads as its base type, so C tools and the compiler treat it
normally; the decompiler uses it to know what a value is.

| Kind | Shown as |
|---|---|
| `message`, `message_file`, `directory` | dialogue comments (above) |
| `item_id` | `ITEM_` constant named from `menu/cfg7/comdatmes<L>.cfg` |
| `monster_kind` | `MONSTER_` constant named from the executable's monster table |
| `map_id` | comment with the place name from `map/map<L>.cfg` |
| `npc_id` | `NPC_` constant named from `npc<L>.cfg` |
| `chara_pack` | comment with the name of the character or monster whose model pack it is, from `npc<L>.cfg` and the monster table; other file names are glossed in `strings/<language>.json` |
| `flag` | a constant from any enum named `flag` or `flag_*` |
| an enum type | a constant from that enum (`enum chara_motion_flag flags`) |

A kind takes constants from every enum that shares its name, so a content
header can add `enum flag_story { ... }` and the SDK's engine flags and the
story flags both print as names.

Kinds spread into script functions. A function that passes its parameter
straight to a kinded parameter gets that kind for the parameter, repeated
until nothing changes, so literals passed to `give_item_to_player(0x51)`
are named the same way as literals passed to the native it wraps.

Names that are the game's own text (items, monsters, NPCs) cannot ship in a
definition set, so decompiling a disc generates them: the decompiler writes
`include/disc.h` into the source tree with `enum item_id`, `enum
monster_kind` and `enum npc_id`, read from that disc in the set's language,
and every script includes it. Literals of those kinds then print as
constants (`place_monster(MONSTER_SEWER_RAT, ...)`, `add_item(ITEM_ROYAL_SWORD)`);
`compile` and `build` find the header through the source tree's `include/`
folder. Without a disc the same kinds fall back to comments.

The game's asset names (motions, texture animations, effects, map parts,
damage settings) are Japanese strings the engine looks up by name. A
whole-disc decompile also writes a `#define` into `disc.h` for each one the
scripts pass to such a parameter, named from the set's English gloss
(`#define MOTION_STAND "立ち"`), and the scripts use the names:
`set_chara_motion(CHARA_PLAYER, MOTION_STAND)`. Names that read the same
are numbered (`MOTION_STAND_2`). A single script decompiled on its own
keeps the strings, with the gloss as a comment.

Character slots cannot be named per map: villagers fill slots in an
order that changes with the story (see `notes/characters.md`); the
villager ids scripts look them up by are `enum npc_id`. Music files and
sound sets have enums in the PAL set's `pal.h` (see `notes/sounds.md`).

## Rules that keep it safe

- Definitions only change how code is shown, never what it compiles to.
  `verify` decompiles with every definition applied and requires the same
  bytes back.
- A constant replaces a number only where the number was one literal, so
  the bytecode stays identical.
- Names that clash fall back to suffixed or default names rather than
  produce broken code.

## Workflow

The easiest way to write definitions is to edit decompiled code:

1. `dc2tk setup dc2.iso` decompiles the disc (its image, or an extracted
   disc) with the shipped sets.
2. Rename functions, parameters, locals and globals, and add doc comments
   above functions and at the top of scripts, in `extracted/scripts/`.
3. `dc2tk defs learn extracted/scripts definitions/pal` compiles the edited files,
   fingerprints each function, and records every name, doc and global name
   that differs from the decompiler's defaults, and the file name, top
   comment and global names of each library file (into `libraries.json`). Because names do
   not change bytecode, any edit that still compiles to the same function
   teaches the definition set. Docs come back wrapped at 100 columns, so an annotated
   script written another way decompiles to the same words on other lines.
   Constants are renamed with `dc2tk defs rename` instead (see content headers).
4. Commit the definition set.

To see where things stand:

- `dc2tk defs disc extracted/disc definitions/pal` records the disc the set is
  for in `disc.json`; `decompile` then warns when it is given a different
  disc, and `defs check` says whether the disc matches.
- `dc2tk defs check extracted/disc` reports how much of the disc the set
  names, per native table, by function and by distinct class, and entries
  that match nothing on the disc.
- `dc2tk defs todo extracted/disc` lists what is still unnamed, most called
  first, with a file and current name to look at.

## Learning from edited source

Names, docs and global names you write in decompiled source can be fed back
into a definition set, so everyone's next decompile gets them:

```
dc2tk defs learn extracted/scripts definitions/pal
```

`learn` compiles each script, matches functions by their bytecode
fingerprint, and records any name, parameter or local name, doc comment, or
global name that differs from the decompiler's defaults; a variable's value
kind or enum (`bgm_file track;`), a changed return type, a `// note: ...`
comment above a statement, and the file name, top comment and global
names of each library. A function that
appears in several files takes the copy you edited; the aliases the
decompiler gives a second copy in one file (`start_event_for_program_506`,
`unused_do_nothing`, `unlock_interrupts_for_recoil_from_hit`) are not
learned as names.

Constants (flag names, the music and sound enums) live in the set's
`include/pal.h`, which trees include from the set, so they are not learned.
`defs rename` renames one in the header, in what the set says about it
(docs, notes, the variables named after it) and in the source of each tree
or mod given with `--in`, keeping unedited files unedited:

```
dc2tk defs rename definitions/pal FLAG_S18_ARRIVAL_SEEN FLAG_YORDA_TREE_ARRIVAL_SEEN --in extracted/scripts
```

## What people will want to do

- Name and document functions, parameters, locals, globals and scripts.
- Say what numbers mean: flags, items, characters, maps, sounds, menus.
- Read English beside Japanese asset names and beside every line of
  dialogue, in the language of their choice.
- Organise output: library names and folders.
- Annotate behaviour: notes on specific lines, known bugs, unused code.
- Target another version: the NTSC or Japanese disc, Dark Cloud 1.
- Ship a mod with a set that extends `pal`, so others can decompile it.
- Check a set against a disc and see how much is still unknown.

## The mod schema

`mod.schema.json` is not a definition set: it is the JSON schema of a mod's `mod.json`
(format 2), for editors that complete and check JSON. `dc2tk` reads `mod.json` into typed
records that refuse unknown keys, and a unit test keeps this schema's keys the same as those
records'.
