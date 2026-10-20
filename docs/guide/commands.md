# CLI Reference

Run `dc2tk help <command>` to see all options for any command.

> **Disc input:** Any command that takes `dc2.iso` also accepts `extracted/disc` (created by `dc2tk setup`). Keep `extracted/disc` unedited if you do: `diff` and `mod make` compare your edits against it, and `build` packs it as the base of the new image.
>
> **PowerShell:** run `.\dc2tk` instead of `dc2tk`.

## Setup and building

| Command | Action |
| --- | --- |
| `dc2tk setup dc2.iso` | Extract the disc, decompile all scripts, and export assets, maps, and data tables into `extracted/`. |
| `dc2tk build extracted/scripts dc2.iso -o modded.iso` | Build a playable ISO from `extracted/scripts` and any `--mod` flags. |
| `dc2tk build extracted/scripts dc2.iso -o test.iso --mod mymod --play` | Build with a mod and launch the ISO in PCSX2. |
| `dc2tk build extracted/scripts dc2.iso -o test.iso --start-at s51:story_50` | Build an ISO where **New Game** jumps directly to map `s51` at program `story_50`. |
| `dc2tk build extracted/scripts dc2.iso --dry-run --mod mymod` | Validate that a mod compiles and fits in memory without writing an ISO file. |
| `dc2tk play modded.iso` | Launch an ISO in PCSX2 (skips PS2 boot logos; pass `--full-boot` to keep them). |

`build` also takes `--mods <dir>` (the folder that holds the mods), `--no-start-at` (ignore a mod's `start_at`), and `--full` (compile every script, not only the edited ones). `--play` and `play` find PCSX2 through `--pcsx2 <exe>`, the `PCSX2` environment variable, your `PATH`, or its usual install folder.

`dc2tk build extracted/scripts dc2.iso --native` builds the SDL3 port for Linux or macOS as
`build/port/darkchronicle`. Pass `--native FILE` to choose its path. Add `-o modded.iso` to
write the ISO too; both builds use the chosen mods, executable tables and native declarations.
The native build needs clang 21, binutils (llvm-objcopy and llvm-nm on macOS), Python 3 and SDL3 development files. A local SDK in
`build/port/sdk` takes precedence over `pkg-config sdl3`; `python3 -I tools/port_sdk.py`
installs that SDK. Run the binary with `--graphics modded.iso`. It warns when the
mounted disc's executable differs from the one used for the build. For MPEG2 movies, run
`python3 -I tools/movie_sdk.py` (or install FFmpeg for `pkg-config`) and add `--movies` to the native build. Engine mods are checked
with clang as well as MWCC by `mod check --native`. A mod cannot override a function supplied by `port/src`.

## The engine

The game's executable (`SCES_511.90`) built from [ChronicleTwo](https://github.com/TheMoonPeople/ChronicleTwo)'s decompiled engine, which this repository pins as the submodule `engine/`. Building needs Docker or Podman (and bash): ChronicleTwo's own build runs in its Kazarov container, on a copy of the engine in `build/engine/`, so `engine/` itself is never changed.

| Command | Action |
| --- | --- |
| `dc2tk engine fetch` | Check out the engine at the pinned commit: the submodule in a clone of this repository, else a clone of ChronicleTwo in `engine/` (needs git). |
| `dc2tk engine build dc2.iso -o SCES_511.90` | Build the executable. The disc (its image, an extracted disc, or its `SCES_511.90`) gives the libraries the build takes from retail. |
| `dc2tk engine verify dc2.iso` | Build the unedited engine and check it against the disc's executable. |
| `dc2tk build extracted/scripts dc2.iso -o test.iso --executable SCES_511.90` | Build an image with an executable `engine build` wrote. When it has outgrown the disc's, it moves past the end of the volume. |

A chosen mod's `engine/` files are built automatically by `dc2tk build … --mod NAME`, including
`--dry-run`. `dc2tk mod check extracted/scripts NAME --disc dc2.iso` builds them too. These
commands need the same container tools as `engine build`; `mod check --native` also builds the port, which needs the native
clang/SDL3 tools, and `--disc` for the retail libraries. Engine mods cannot be combined with `--executable`. See [Engine files](mods.md#engine-files)
for function replacement, `original()`, headers, added units and native declarations.
`dc2tk mod override engine NAME FUNCTION` copies an engine function into a mod and refuses
an existing destination. Engine mods can also edit executable data tables; those values are
compiled from source. The build reports the space between BSS and the
PS2 stack and fails if BSS reaches past the stack.

The first build makes the container image, which takes a while; later builds reuse it and the copy, and recompile only what changed. `--engine DIR` builds another ChronicleTwo checkout, such as one you have edited, in a copy of its own. The patches in `tools/engine-patches/ps2.json` are applied to every copy: they let an edited engine link (no address fixed to retail's layout, the compiler's alignment kept), and change nothing in an unedited build, which `engine verify` checks.

## Creating and managing mods

| Command | Action |
| --- | --- |
| `dc2tk diff dc2.iso` | List all scripts, assets, and disc files modified in `extracted/`. |
| `dc2tk changes extracted/scripts` | List only `.c` scripts modified in `extracted/scripts/`. |
| `dc2tk mod make dc2.iso <name>` | Package the edited scripts, assets, and disc files in `extracted/` into `mods/<name>/` (edits in `extracted/data` and `extracted/maps` are not packaged). |
| `dc2tk mod asset dc2.iso <name> <id>` | Copy an asset or data table into `mods/<name>/` and register it in `mod.json` (for example, `data:items` or `texture:e254a1`). |
| `dc2tk mod new extracted/scripts <name> <file>` | Copy whole script files into `mods/<name>/files/` to edit there. |
| `dc2tk mod save extracted/scripts <name>` | Copy every script file edited or added in `extracted/scripts` into `mods/<name>/`. |
| `dc2tk mod override extracted/scripts <name> --map s51 <func>` | Create a patch in `mods/<name>/` that overrides `<func>` (also accepts `--monster`, `--script`, or `--library`). |
| `dc2tk mod check extracted/scripts <name>` | Compile and validate a mod without building an ISO (pass `--disc dc2.iso` to also check assets and data tables). |
| `dc2tk mod test extracted/scripts <name>` | Run unit tests in `mods/<name>/tests.c` inside the built-in script VM. |
| `dc2tk mod list extracted/scripts` | List all mods in `mods/`, their patches, and allocated IDs. |
| `dc2tk mod slim extracted/scripts <name>` | Convert full-file script copies in a mod into small function patches. |
| `dc2tk mod header extracted/scripts <name>` | Regenerate `.editor/` headers and `compile_commands.json` for `clangd`. |
| `dc2tk mod apply extracted/scripts <name>...` | Write mods' files into a source tree. |
| `dc2tk mod watch extracted/scripts dc2.iso <name>...` | Rebuild mods into the native port's override folder whenever they change and make the running debug build reload them (see [Hot reloading](mods.md#hot-reloading-in-the-native-port)). |

## Searching and inspecting the game

| Command | Action |
| --- | --- |
| `dc2tk maps extracted/scripts` | List every map code, English name, and type (town or dungeon). |
| `dc2tk map extracted/scripts <map>` | Show a map's programs, free program numbers, triggers, and spawn coordinates (`--json` for JSON). |
| `dc2tk map-export <map>` | Export a map to `extracted/maps/<map>/` as a `.gltf` scene (`-o <dir>` writes it elsewhere, such as into a mod; `--all` exports all maps; `--list` lists them). |
| `dc2tk xref extracted/scripts map <map>` | Show every script that warps into `<map>` and which program it starts. |
| `dc2tk xref extracted/scripts monster <name>` | Show a monster's 3D model, AI script, and spawn scripts. |
| `dc2tk xref extracted/scripts event <num> --map <map>` | Show what defines and triggers event program `<num>` on `<map>`. |
| `dc2tk flags extracted/scripts [--flag <n>]` | Show used and free save flags (`--used` lists every used flag; `--counters` and `--counter <n>` do the same for save counters). |

## How key workflows work

### Decompiling

`dc2tk setup` decompiles all scripts on the disc into `extracted/scripts/`. Shared functions are written once to `extracted/scripts/lib/` and included with `#include "lib/..."`. File hashes are saved in `original-hashes.json`; `setup` and `decompile` do not overwrite edited files unless you pass `--force`.

To decompile one `.stb` file manually, run `dc2tk decompile file.stb -o file.c`.

### The asset tree

`dc2tk setup` exports each unique texture (`.png`), character model (`.gltf`), sound effect (`.wav`), music sequence (`.mid` + `.sf2`), and menu text file (`.txt`) into `extracted/assets/`. `extracted/assets/manifest.json` maps every asset ID (`texture:e254a1`, `model:c02fuku1`, `sound:SE_CURSOR_MOVE`, `bank:SP_003`, `music:BG_000_000`, `text:en/shopname`) to every disc archive that contains it. Commands that take an id also take a pattern such as `model:c02fuku*`.

To regenerate `extracted/assets/`, run `dc2tk assets dc2.iso --force`.

### Compiling

`dc2tk compile file.c -o file.stb` compiles one `.c` file to `.stb` bytecode. You do not need to run `compile` manually during normal modding because `dc2tk build` and `dc2tk mod check` compile modified scripts and patches automatically.

### Building

`dc2tk build extracted/scripts dc2.iso -o modded.iso` compiles only modified scripts and `--mod` patches, converts modified assets, checks all changes against PlayStation 2 [Memory Limits](memory.md), and writes `modded.iso`. Unmodified files remain byte-for-byte identical to the retail disc.

### Testing without replaying

Pass `--start-at <target>` to `dc2tk build` (or set `"start_at"` in `mod.json`) to make **New Game** warp directly to a map or event:

| Target | Result on New Game |
| --- | --- |
| `--start-at m01` | Spawn in Palm Brinks (`m01`) at `map_start` |
| `--start-at s51:story_50` | Run program `story_50` (`10500`) on map `s51` |
| `--start-at s06/s06b:500` | Run program `500` of map `s06`'s script `s06b` |

`build` warns when the game never enters the map that way, so the warp may not work. Flag `2047` is reserved for `--start-at`.

## Low-level commands

| Command | Action |
| --- | --- |
| `dc2tk extract dc2.iso extracted/disc` | Unpack raw disc files without decompiling scripts or converting assets. |
| `dc2tk pack extracted/disc -o copy.iso` | Repack raw disc files into an ISO (`--check dc2.iso` compares the result byte for byte). |
| `dc2tk disassemble file.stb` | Disassemble `.stb` bytecode to text assembly. |
| `dc2tk assemble file.asm -o file.stb` | Assemble text assembly into `.stb` bytecode. |
| `dc2tk fork extracted/scripts lib/old.c lib/new.c script.c` | Split a shared library file for specific scripts. |
| `dc2tk verify extracted/disc` | Verify that all 1,398 retail scripts decompile and recompile byte-for-byte (`--json <file>` writes the full report). |
| `dc2tk defs learn` / `check` / `todo` / `rename` / `disc` | Maintain the decompiler's naming definitions (see [definitions/README.md](../../definitions/README.md)). |
| `dc2tk natives` / `dc2tk api-docs` | Regenerate the native tables and the [API Reference](../scripting/api/README.md). |
