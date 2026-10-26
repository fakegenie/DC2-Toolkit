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
| `dc2tk build extracted/scripts dc2.iso -o test.iso --mod local.mymod --play` | Build with a mod and launch the ISO in PCSX2. |
| `dc2tk build extracted/scripts dc2.iso -o test.iso --start-at s51:story_50` | Build an ISO where **New Game** jumps directly to map `s51` at program `story_50`. |
| `dc2tk build extracted/scripts dc2.iso --dry-run --mod local.mymod` | Validate that a mod compiles and fits in memory without writing an ISO file. |
| `dc2tk build extracted/scripts dc2.iso -o modded.iso --pak other-mod-1.0.0.dc2pak` | Put a built mod on the image; repeat `--pak` for more (see [Playing built mods](mods.md#playing-built-mods)). |
| `dc2tk build extracted/scripts dc2.iso -o modded.iso --set my-game.json` | Build the mods a [mod set](mods.md#mod-sets) chooses, with its settings; its numbers go in `my-game.lock`. |
| `dc2tk play modded.iso` | Launch an ISO in PCSX2 (skips PS2 boot logos; pass `--full-boot` to keep them). |
| `dc2tk play dc2.iso --native --set my-game.json` | Run the native port over the retail disc with a mod set's game files (`--build-only` prints the command instead). |

`build` also takes `--mods <dir>` (the folder that holds the mods), `--dev` (use the chosen mods' `"dev"` settings: New Game starts at their `start_at`), and `--full` (compile every script, not only the edited ones). `--play` and `play` find PCSX2 through `--pcsx2 <exe>`, the `PCSX2` environment variable, your `PATH`, or its usual install folder.

`dc2tk build extracted/scripts dc2.iso --native` builds the SDL3 port for Linux or macOS as
`build/port/darkchronicle`. Pass `--native FILE` to choose its path. Add `-o modded.iso` to
write the ISO too; both builds use the chosen mods, executable tables and native declarations.
The native build needs clang 21, binutils (llvm-objcopy and llvm-nm on macOS), Python 3 and SDL3 development files.
`python3 -I tools/port_sdk.py` builds SDL3 and FFmpeg's MPEG-2 decoder from pinned sources into `build/port/sdk` (it needs
cmake, make and, on Linux, SDL's build dependencies), which takes precedence over `pkg-config`; `tools/port.py` copies
their libraries into `build/port/lib` beside the binary. Run the binary with a disc image, `build/port/darkchronicle modded.iso`, to start
the game straight away; it warns when the mounted disc's executable differs from the one used for
the build. Run it without one to open the launcher, where the disc, window, widescreen, high frame
rate, sound and, in the debug build, the debug panel are chosen, and built mods are turned on and
off (see [Playing built mods](mods.md#playing-built-mods)). It keeps them in `launcher.cfg` in
the user's settings folder (`~/Library/Application Support/DC2 Toolkit/Dark Chronicle/` on macOS,
`~/.local/share/DC2 Toolkit/Dark Chronicle/` on Linux), and options given with it still win. Saves go in `saves` in that
folder (or `Dark Chronicle/saves` in iCloud Drive, when iCloud Drive has a `Dark Chronicle` folder), unless
`DC2_SAVE_ROOT` names another. Built mods go in `mods` and overrides in `overrides` beside the binary, or in the settings
folder for `Dark Chronicle.app`. For MPEG2 movies add `--movies` to the native build. Engine mods are checked with clang
as well as MWCC by `mod check --native`.

`python3 -I tools/port_package.py` builds the port with movies and packs it for players in `build/port/package`:
`Dark Chronicle.app` in a zip on macOS, `darkchronicle` with its libraries in a tar.gz on Linux, or `Dark Chronicle.exe`
with its DLLs in a zip on Windows, each with
`port/package/README.md`, `port/package/NOTICE.md`, the LICENCE and the licences of SDL3, FFmpeg, the launcher's font
and the other code built in. `--ios` packs the unsigned `--ios device` app, with the notice and licences inside it, as
`dark-chronicle-<version>-ios.ipa` for sideloading. `--android` packs `dark-chronicle-<version>-android-arm64.apk`,
with the notice and licences in its assets, signed with the public key in `port/android` so each release installs over
the last. The release workflow runs all of them for every tag.

`python3 -I tools/port.py --ios simulator` (or `--ios device --team TEAM`, signed for an iPhone or iPad, or `--ios device` alone, unsigned) builds `DarkChronicle.app` from SDKs `python3 -I tools/ios_sdk.py` installs. The app opens on the launcher. Its Disc row picks a disc image anywhere the Files app reaches; without one it uses the first `.iso` in the app's own folder (Files, On My iPhone, Dark Chronicle). Built mods go in that folder's `mods`, which the Mods page's Open mods folder shows in Files. The Saves row chooses where saves are kept: `saves` in that same folder by default, or a folder anywhere Files reaches, such as iCloud Drive, where they go in a `Dark Chronicle` folder. Picking the app's own folder goes back to the default. Its settings leave out the window, vsync and the debug panel, offer high frame rates up to 120 Hz and an aspect ratio of Fill screen (the screen's own shape, `--aspect fill`), and add On-screen controls: a touch D-pad, both sticks, the face and shoulder buttons, Select and Start, drawn over the game. They are Off, On, or Smart (the default), which shows them only while no controller is connected. The sticks float: a touch on the left half of the screen that misses every button becomes the movement stick where it lands, and one on the right half the camera stick. A pressed button glows in its colour and taps the phone (Haptics turns that off), and the controls dim after a few seconds without a touch. Save state buttons (on by default) put Save State and Load State between the shoulder buttons; with a controller connected, or the controls off, a tap anywhere shows them for a few seconds. There is one state, `quick.dc2state` in the saves folder, so it outlasts closing the app. The app keeps a private copy of the build that made it, so the state still loads after an update until the next save replaces it. A mod cannot override a function supplied by `port/src`.

`python3 -I tools/port.py --android` builds `build/port/android/libmain.so` for arm64 Android 10 or later from the SDKs
`python3 -I tools/android_sdk.py` installs (SDL3 shared, FFmpeg static, both built with the NDK from `ANDROID_NDK_HOME`
or the newest in `ANDROID_HOME/ndk`), and `tools/port_package.py --android` packs it with SDL's Java activity and
`port/android` (the manifest, `DarkChronicle.java` and the key) into an APK, built with javac, d8, aapt2, zipalign and
apksigner from the newest Android build tools, no Gradle. The engine compiles with clang 23 against the NDK's sysroot,
as the NDK's own clang has no `#embed`, and the NDK's clang++ links. The app opens on the same launcher and settings as
on iOS. Its Disc row opens Android's file picker and remembers the disc it picks, and the Mods page's Add a mod copies a
picked `.dc2pak` into the mods folder. Saves, mods and `log.txt` (what the port prints) are in the app's folder,
`Android/data/dc2toolkit.darkchronicle/files`; `launcher.cfg` is in its private storage. The launcher draws with SDL's
Vulkan renderer, like the game. A state saved before an update does not load after it.

On Windows (x64, Windows 10 1903 or later), `python -I tools/port_sdk.py` first unpacks the pinned llvm-mingw (LLVM 23)
into `build/port/llvm-mingw`, then builds SDL3 and FFmpeg with it into `build/port/windows-sdk`. FFmpeg's configure runs
in Git for Windows' bash, so it needs Git, cmake and Python. `python -I tools/port.py --movies` then builds
`build/port/windows/darkchronicle.exe` with `SDL3.dll`, `avcodec-62.dll` and `avutil-60.dll` beside it, and
`tools/port_package.py` packs it as `Dark Chronicle.exe` in `dark-chronicle-<version>-windows-x64.zip`. It draws with
Vulkan, its manifest makes the C runtime's file names UTF-8, and it opens no console of its own but writes to the one it
is started from. `launcher.cfg` and `saves` are in `%APPDATA%\DC2 Toolkit\Dark Chronicle`, built mods in `mods` beside
the exe. `python -I port/tests/run.py` runs the boundary checks there too. The debug build (`--debug`) and
`dc2tk build --native` do not run on Windows yet.

A chosen mod's `"port"` sources are compiled into the binary too (see [Native port mods](mods.md#native-port-mods)); `python3 -I tools/port.py --mod mods/<name>` does the same for the port's own build.

## The engine

The game's executable (`SCES_511.90`) built from [ChronicleTwo](https://github.com/TheMoonPeople/ChronicleTwo)'s decompiled engine, which this repository pins as the submodule `engine/`. Building needs Docker or Podman (and bash): ChronicleTwo's own build runs in its Kazarov container, on a copy of the engine in `build/engine/`, so `engine/` itself is never changed.

| Command | Action |
| --- | --- |
| `dc2tk engine fetch` | Check out the engine at the pinned commit: the submodule in a clone of this repository, else a clone of ChronicleTwo in `engine/` (needs git). |
| `dc2tk engine build dc2.iso -o SCES_511.90` | Build the executable. The disc (its image, an extracted disc, or its `SCES_511.90`) gives the libraries the build takes from retail. |
| `dc2tk engine verify dc2.iso` | Build the unedited engine and check it against the disc's executable. |
| `dc2tk build extracted/scripts dc2.iso -o test.iso --executable SCES_511.90` | Build an image with an executable `engine build` wrote. When it has outgrown the disc's, it moves past the end of the volume. |

A chosen mod's `engine/` files are built automatically by `dc2tk build … --mod NAME`, including
`--dry-run`. `dc2tk mod check extracted/scripts ID --disc dc2.iso` builds them too. These
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
| `dc2tk mod new extracted/scripts <name> [<file>...] [--id author.name]` | Start a mod in `mods/<name>/` with its `mod.json`, copying any whole script files given into its `files/` (listed under `"scripts": {"whole"}`). |
| `dc2tk mod save extracted/scripts <name>` | Copy every script file edited or added in `extracted/scripts` into `mods/<name>/`. |
| `dc2tk mod override extracted/scripts <name> --map s51 <func>` | Create a patch in `mods/<name>/` that overrides `<func>` (also accepts `--monster`, `--script`, or `--library`). |
| `dc2tk mod check extracted/scripts <id>...` | Compile and validate a mod without building an ISO (pass `--disc dc2.iso` to also check assets and data tables). |
| `dc2tk mod test extracted/scripts <id>` | Run unit tests in the mod's `tests.c` inside the built-in script VM. |
| `dc2tk mod list extracted/scripts` | List all mods in `mods/`, their patches, and allocated IDs. |
| `dc2tk mod slim extracted/scripts <name>` | Convert full-file script copies in a mod into small function patches. |
| `dc2tk mod header extracted/scripts <name>` | Regenerate `.editor/` headers and `compile_commands.json` for `clangd`. |
| `dc2tk mod apply extracted/scripts <id>...` | Write mods' files into a source tree. |
| `dc2tk mod watch extracted/scripts dc2.iso <id>...` | Rebuild mods into the native port's override folder whenever they change and make the running debug build reload them (see [Hot reloading](mods.md#hot-reloading-in-the-native-port)). |
| `dc2tk mod pack <id> [-o DIR] [--disc dc2.iso]` | Pack a mod into `<id>-<version>.dc2mod`, a zip of its files that is the same bytes every time (see [Sharing a mod](mods.md#sharing-a-mod)). |
| `dc2tk mod install <file>.dc2mod` | Copy a packed mod into the mods folder, replacing an older version of it. |
| `dc2tk mod upgrade <folder> [--id ID]` | Turn a mod from before format 2 into a format 2 mod in place, numbers included (see [Upgrading an old mod](mods.md#upgrading-an-old-mod)). |
| `dc2tk mod build <tree> <disc> <id> [-o DIR]` | Build a mod into `<id>-<version>.dc2pak`, the game files it changes, for the native port's launcher (see [Playing built mods](mods.md#playing-built-mods)). |

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

Pass `--start-at <target>` to `dc2tk build` (or set `"dev": {"start_at": ...}` in `mod.json` and build with `--dev`) to make **New Game** warp directly to a map or event:

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
