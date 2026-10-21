# Plan

The engine in dc2tk, mods that change it, and a minimal native port for macOS and Linux, with
Windows to follow. The facts about
ChronicleTwo come from its source at commit `d51e74b` (Kazarov at `14164f1`), given as paths under
the submodule, `engine/...`.

## Picking this up

### Where things stand

**The port no longer emulates the PS2** (2026-10-20, uncommitted on `engine`). The DMA, VIF, VU1
and software GS are deleted. The engine's drawing calls now reach an SDL_GPU renderer (Metal on
macOS, Vulkan elsewhere), and `CSound` calls the synth directly with no IOP behind it. It builds
and runs on macOS arm64 with 0 stubbed symbols, at 50 Hz in a real window with real sound: the
language select, title, the attract demo, New Game, the costume screen, the opening cutscene,
the castle dungeon with its HUD, and the main and item menus. `port/tests/run.py` passes on macOS.
Linux compiles and links (checked in Docker, aarch64) but has not been run since. Part 2 below
describes the port as it is now; the table and notes after this paragraph are the earlier review
of the emulated port and are kept for the engine milestones and the fixes.

Reviewed 2026-10-20, after an agent worked overnight from the tasks this file held. Its work
(about 15,700 new lines and 1,850 changed) is committed on the branch `engine`. "Checked in
review" means rerun on 2026-10-20; the rest is the overnight agent's record, with its evidence
under `build/` (not in git).

| Milestone | Built | Checked in review | Open |
|---|---|---|---|
| E1 the engine as a submodule | yes | `engine verify dc2.iso`: every section retail's | |
| E2 a build an edited engine relinks in | yes | | the PCSX2 boot of the 50,000-money edit |
| E3 engine mods for the ISO | yes, with `original()`, replaced headers, new units and `mod override engine` | Rust tests; `tools/test_engine_mods.py` | PCSX2 play; fixes 3, 4, 13 |
| E4 executable tables and natives from source | yes, every table but `data:parts` | Rust tests | `data:parts`; fixes 4, 5, 14 |
| P0 compile spike | yes | | |
| P1 headless boot | yes | boots `dc2.iso` to the language selector | |
| P2 graphics | the engine draws through SDL_GPU (replaced the emulated DMA, VIF, VU1 and GS) | `tools/port.py --movies` builds clean (149 units, 40 files, no stubs); `port/tests/run.py` passes on macOS; 50 Hz in a window | PCSX2 comparison; water on screen; lens flare, depth of field and texture animation on screen |
| P3 input | SDL keyboard and gamepad as a DualShock 2 | | a human playing it |
| P4 sound | SPU2 voices, ADSR, reverb, the MIDI sequencer; `CSound` calls them directly | the port suite's sound and MIDI checks | listening against retail; a real audio device |
| P5 saves and movies | memory cards as folders; MPEG-2 movies | the port suite's card checks; the retail logo (150 frames) and tutorial (1,275) movies decode | console-made saves |
| P6 native mods | `dc2tk build --native`, the executable hash check, `mod check` with clang | `tools/test_native_prepare.py` | PCSX2 parity; fixes 3, 6, 11 |

What the overnight agent recorded, and which the review did not rerun:

- **Play, through a replay harness** (`port/tests/gameplay.py`, controller reports injected at
  the input boundary, SDL's dummy video and audio drivers): the castle tutorial and its soldiers;
  Palm Brinks and Cedric's workshop; a save through the game's own menu and Continue in a new
  process (416,192-byte saves, the retail record layout); the circus clowns; the whole opening
  movie, `OPEN1.PSS`; Underground Channel floor one with combat, item use and the key. Melee hits
  in the corridor and the floor's exit are unchecked. Approaching the gate crashed (SIGSEGV); the
  effect-heap substitutions in `port.json` fix the cause found in the core dump, but no run has
  reached the gate since.
- **Native bugs found and fixed by play:** a texture whose TEX0 stride is zero; the minimap's
  `PartsInfoData` loop running past its end; monster script arenas too small for native record
  sizes; streamed audio stuck at END; effect and subcharacter heap reservations too small.
- **E3, E4 and P6 together:** one fixture mod (a dungeon override, free and member `original()`,
  a replaced header, a new unit, a Donny shop edit, a new native and a texture) builds both the
  ISO and the native binary; the binary records the ISO executable's hash, the native call
  returns its status, and the replaced title picture shows.
- **The golden run** (`tools/golden.sh`) passed on the full disc: 18,874 files match the baseline
  and every image built with nothing edited keeps retail's executable.
- **Speed:** the software GS runs below 50 frames a second. Measurements, traces and a serial
  reference renderer are under `build/validation/gs-performance`.
- **The machine rebooted** at 21:24 on 2026-10-19 during a long trace capture, cause unknown.
  Docker's images were gone afterwards (rebuilt in review). Run one heavy workload at a time on
  four CPUs (`taskset -c 0-3`, `DC2TK_GS_THREADS=4`).
- **No PCSX2 or PS2 BIOS** is available on this machine (the user confirmed), so every comparison
  with PCSX2 is the user's.

Save checkpoints to resume play from, in `build/validation/crash-recovery/`: `first-game-save`
(Palm Brinks), `before-circus-save`, `underground-entrance-save` and
`first-floor-selection-save`.

### Fixes, in order

The review read every new file and found no decompiled code copied into the repository: the 137
substitutions in `port.json` are one to four lines each, and the port's 10 replaced engine
functions (8 in the movie code, `WaitVSync`, `mgSetRotateThread`) and its asm replacements are
written fresh. These are what it found to fix:

1. **Done:** the overnight work is committed on `engine`, unchanged from what the review checked:
   the native tables' parser, the port, engine mods for the ISO and the port, and this plan.
   `build/` stays out of git; it holds the evidence above.
2. **Done: the port's allocator.** The engine's `operator new` and `delete` are renamed in its
   objects to a 64 MB arena of its own, which reports when it runs out; the rest of the process
   uses the system allocator. What follows is the review's original finding.
   `port/src/memory.c:174-245` replaces `malloc`, `free`, `calloc`,
   `realloc`, `posix_memalign` and `aligned_alloc` for the whole process with a 64 MB arena at
   `0x08000000` that SDL, PulseAudio, Mesa and libavcodec share; `memalign`, `valloc` and
   `pvalloc` still reach glibc, so a library calling them hands our `free` a foreign pointer;
   `free` walks every block under a spinlock the GS workers also take; the startup check accepts
   an `_end` up to `0x10000000` though the arena needs it below `0x08000000`; and an exhausted
   arena returns NULL without a word. Only the engine needs low addresses: give the engine's
   objects their own allocator (`objcopy --redefine-sym` of `malloc`, `free`, `operator new` and
   `delete` in engine objects, as `main` already becomes `port_game_main`), leave the rest of the
   process on glibc, check `_end` against the arena, and stop with a message when it runs out.
3. **Done: `mod check --native`.** `mod check` always built the native port for engine mods (`modding/check.rs:57-66`),
   which fails on Windows, macOS and any Linux without SDL3, and costs a full native compile.
   Make the native check `--native`, as `build` has it.
4. **Units whose tables are written as source lose Satan's Fiddle's profile**
   (`engine.rs:526-547`), so a `data:monsters` edit beside any engine mod recompiles all of
   `monster.cpp` without retail's codegen. Keep the profile for units whose only change is
   tables, and after the link compare each edited table's bytes in the built ELF (found through
   the `.xMAP`) with the bytes the byte-patch path would have written.
5. **`data:parts` has no source path** (`engine_tables.rs:35-196`): `PartsInfoData`, the part
   names at `0x36C708` and `MiniMapInfoData`. It is refused loudly today; implement it, with a
   test that pins it.
6. **`--native`'s default path** (`modding/build.rs:63`) is relative to the current folder, not
   the repository's `build/`; and `--native FILE --dry-run` ignores FILE.
7. **One native build.** `tools/port.py` and `src/crates/dc2tk/src/native.rs` each carry the
   flags, aliases, localised symbols, the GS bundle and Kazarov's `libvu0.cpp`. Keep them in
   dc2tk and make `tools/port.py` a thin call to `dc2tk build --native` (or drop it), so they
   cannot drift.
8. **CPU use.** `tools/movie_sdk.py:76` runs `make -j8`; it should follow the affinity. (The GS
   workers this also named are gone.)
9. **Debugging in the shipping binary:** the screenshot hook (`present.c`, `DC2TK_SCREENSHOT`),
   scripted presses (`input.c`, `DC2TK_PRESS`), the sound dump (`audio.c`, `DC2TK_SOUND_DUMP`),
   `cdvd.c`'s 1 MB index scanned on every read to log archive members, and the log line for every
   file opened. Move them into the test build (as the replay wrapper is) or
   behind one logging switch.
10. **Done:** `dma_chain.c` is deleted with the rest of the emulation.
11. **`port/tests/run.py:59-80`** writes patched engine sources into a temporary folder under
    `/tmp`; write them under `build/port/tests/` instead. `tools/port.py:129` gives every unit
    `-I engine/port/src`, which only the four upstream asm units need. `-DPORT_SOUND` is always
    on, so its conditionals switch nothing: remove them.
12. **The effect-heap substitutions** (`port.json` #132-136): with PS2 sizes the new formula gives
    166 quadwords against retail's 161; #135 adds an unexplained `+ 3` no test covers; #136
    changes nothing on x86-64. Make the formula give retail's count with PS2 sizes, test the
    `+ 3` or drop it, and drop #136.
13. **Build copies** have no lock (two builds of one mod set share `build/engine/mods-<hash>`, and
    every native build shares `build/port/native-*`), and old `mods-*` copies are never removed.
    Header mods without a fetched engine fail with a confusing message instead of "run
    `dc2tk engine fetch`".
14. **The native tables' parser now masks strings and comments** (a fix: Shift-JIS bytes in
    `_SHOT`'s strings ran it into later functions), which changes `_SHOT`'s accessors from the
    committed 42 to one. Regenerate `definitions/natives/` with `dc2tk natives engine` and say so
    in the commit; script bytes do not change.
15. **Style:** `native.rs` lacks its `//!` line; `build.rs:63-66` uses `help = "..."` where the
    crate uses `///`; `python_in_container` runs Python without `-I`; `tools/native_prepare.py`,
    `tools/test_native_prepare.py` and `port/tests/native_save.py` lack the usage docstring.
16. **Disk:** `build/` is 33 GB, 22 GB of it `build/validation` (traces and captures). Once the
    work is committed, keep the save checkpoints and delete the traces.

### Gates only the user can run

- E2, E3, E4: the modded images in PCSX2 or on a PS2.
- P2 to P6: the port in a real window with real sound (this machine has run it only with SDL's
  dummy drivers), compared with PCSX2; a human playing it; sustained 50 frames a second.

### Read first

- This file, then [src/docs/development.md](src/docs/development.md) (dc2tk's rule and checks)
  and [src/README.md](src/README.md) (the crates).
- `src/crates/dc2tk/src/engine.rs` (the copies, patches and container builds), `engine_tables.rs`
  (executable tables as source), `engine_natives.rs`, `native.rs` (`build --native`).
- `tools/engine-patches/ps2.json` and `port.json`, `tools/engine_mods.py` (`original()`, headers,
  new units, `mod override engine`) and `tools/native_prepare.py`.
- `port/src`, and `port/tests/run.py` for what each part is checked by.
- ChronicleTwo's `engine/docs/vu1/*.md`, `engine/docs/SOUND_DAT.md`, `engine/docs/MWCC.md` and
  `engine/docs/SATANSFIDDLE.md` when a task reaches them.

### Rules

- **No decompiled code in this repository.** The engine stays in the submodule, and nobody edits
  `engine/`. Every build works on a copy under `build/`. What has to change in the engine's files
  is a substitution in `tools/engine-patches/*.json`, and only token-sized ones in game source
  (a type, a size, one declaration, one literal), never a function body. A larger change is a
  function replaced on the copy, written fresh. Test mods use bodies of our own, never a copy of
  a decompiled one. Decompiled source is written only under `build/`.
- **dc2tk's rule:** a disc image built with nothing edited is retail's, byte for byte
  (development.md). With no engine change, the disc keeps retail's `SCES_511.90`. After any change
  to the PS2 patches, `dc2tk engine verify` must still say every section is retail's.
- **The port runs the game as the PS2 did, natively:** retail formats read from the ISO, the
  original `.stb` scripts on the original VM, PAL timing at 50 Hz, the game's own resolution and
  draw order. No 60 fps, no converted data, no replaced textures, no other scripting language. No
  emulation either: where the engine drove PS2 hardware, the port replaces that engine function
  with one that does the same thing through SDL3, and changes as few functions as it can. From ChronicleTwo's `port/`
  only the four asm files are built, and from Kazarov's `src/cpp/sce` only `libvu0.cpp` (MIT);
  the rest of `sce` may be read for how a library behaves.
- **Test code stays out of the shipping binary:** the replay harness, traces and profiling link
  only into the test builds.
- **Code:** functions and plain data; no classes, wrappers or higher-order helpers in code of our
  own. No comments, except what the conventions need to work: the one-line `//!` module doc and
  clap's `///` help text in Rust, and a usage docstring at the top of a Python tool. The port is
  C (C11) except where an engine type or a mangled name makes C++ necessary.
- **Prose** (docs, commit messages) is plain and direct, in the style of the existing docs and
  `git log`.
- **Ask the user** before installing anything system-wide and before anything that needs sudo.
  Never push, merge or change `main` without the user.

### This machine

- **macOS arm64** (the machine the native rewrite was done on), with Homebrew's `sdl3`,
  `ffmpeg`, `llvm` (for `llvm-objcopy` and `llvm-nm`), `pkg-config`, `glslang` and
  `spirv-cross`, and Rust from rustup. The retail disc is `dc2.iso` in the repository root.
- **Building the port:** `python3 -I tools/port.py --movies`, then `build/port/darkchronicle
  dc2.iso`. Running it needs the window server. Keys: WASD move, F/H/T/G camera, arrows the
  D-pad, I/J/K/L triangle/square/cross/circle, Q/E L1/R1, 1/3 L2/R2, C/V L3/R3, Enter Start,
  Backspace Select. Saves go to `DC2_SAVE_ROOT` (default `./saves`, ignored by git).
- **Shaders:** `port/shaders/*.vert` and `*.frag` are compiled by `python3 -I
  tools/port_shaders.py` into `port/src/shaders.h` (SPIR-V and Metal source), which is committed
  so a build needs neither tool. Run it after changing a shader.
- **Test aids in the binary** (fix 9): `DC2TK_PRESS="cross@300,start@1500/150"` presses a button
  at a field, or every period after it; `DC2TK_SCREENSHOT=file.bmp` with `DC2TK_SCREENSHOT_FIELD`
  and `DC2TK_SCREENSHOT_EVERY` saves the shown frame; `DC2TK_SOUND_DUMP=file.raw` writes the
  mixed sound.
- **Crashes:** the binary re-executes itself at start (see Part 2), which LLDB follows badly.
  macOS writes a report for every crash to `~/Library/Logs/DiagnosticReports/darkchronicle-*.ips`
  (JSON after the first line), with the faulting thread's frames.
- **The debug build:** `python3 -I tools/imgui_sdk.py` once (Dear ImGui 1.92.9b into
  `build/port/imgui-sdk`), then `python3 -I tools/port.py --movies --debug` also links
  `build/port/darkchronicle-debug`. It opens a second window (F1 shows or hides it, F5 pauses, F6
  steps) and serves the same commands on `http://127.0.0.1:7777/` (`DC2TK_DEBUG_PORT`).
  `python3 -I tools/debug.py launch`, then `help`, `status`, `stats frames=200 --summary`,
  `press buttons=cross frames=6`, `step frames=10`, `speed value=0`, `screenshot out.png`,
  `windowshot out.png`, `tab name=Capture`, `profile seconds=3`, `town map=18`, `dungeon dungeon=0 floor=2`,
  `capture`, `toggle name=shadows value=0`, `record path=...`, `replay path=...`, `save slot=1`,
  `load slot=1`. Check the game through `screenshot` and `windowshot`, never a full-screen capture.
- **Golden screenshot tests:** `python3 -I tools/golden_port.py` runs `port/tests/golden/*.json`
  against the debug build, paused and stepped frame by frame; `--update` writes the baselines to
  `build/port/golden/` (game imagery, never in git).
- **Crash sweep:** `python3 -I tools/event_sweep.py` plays every row of the debug menu's event list
  (`event/view_pal.txt`) in its own copy of the debug build (port 7791, throwaway saves) and writes
  `build/port/event-sweep.json` with the top of each crash's backtrace; `--rows 32,129` or `--from 145`
  for a subset. Rows run in order without a save reset, as the in-game list does, so a crash can need
  the rows before it.
- **Frame-time benchmark:** `python3 -I tools/perf_bench.py` measures CPU time per game frame (every
  thread) and busy time per frame (frame time less the wait for the next field) in a fixed set of towns,
  dungeons and boss fights (port 7792) and compares it with the previous run in
  `build/port/perf-bench.json`. Busy time includes the kernel's idle sleeps, so `game_ms` is not engine
  work alone. Compiling the engine at -O2 saves 3-5% of CPU in towns and 17% in dungeons; it stays at -O0.
- **Where the CPU goes** (`debug.py profile`, count `iokit_user_client_trap` as work: it is Metal
  submitting command buffers): game logic is small; the cost is draw submission and render passes
  (47 a frame in Balance Valley, 53 in the Dark Genie fight), then SPU2 mixing on the main thread.
  Done: unchanged GPU state is not resent, queued draws keep the texture they sampled so copies into
  it rename it instead of ending the pass, meshes stay in a GPU buffer, the kernel sleeps to the next
  field. Still open: encoding the whole frame at present (one vertex upload instead of one per pass),
  the add/subtract alternation of shadow volumes (about 2500 commands in the Dark Genie fight), the
  heat-haze frame copies (18 passes in Balance Valley), and characters composited one by one between
  targets 2 and 10.
- **The replay harness:** `python3 -I port/tests/gameplay.py` links
  `build/port/replay/darkchronicle`; `DC2TK_REPLAY_FIELDS`, `DC2TK_REPLAY_SKIP`,
  `DC2TK_REPLAY_FIGHT` and `DC2TK_REPLAY_SCREENSHOTS` steer it.
- **Linux** (the machine of the earlier work): clang 21, binutils, Python 3 and Docker; Rust in
  the `dc2tk-rust` image (`printf 'FROM rust:1-trixie\nRUN rustup component add rustfmt clippy\n' |
  docker build -t dc2tk-rust -`); `python3 -I tools/port_sdk.py` copies SDL3 out of Kazarov's
  image and `python3 -I tools/movie_sdk.py` builds FFmpeg 8.0.1's MPEG-2 decoder.
- **Kazarov's image** `kazarov-dev:01b70a4a6008` is made by `dc2tk engine` when missing (a few
  minutes).
- **The scratch folder under `/tmp` is cleared by a reboot.**

### Checks

- `cargo fmt --all`, then `cargo clippy --release -p <crate> --no-deps --all-targets -- -D warnings`
  for each crate changed, then `cargo test --release -p <crate>` for each, one at a time (a
  failing crate stops cargo before the next).
- Two failures predate this work and are not yours to fix: the `dc2tk-disc` test
  `a_disc_is_not_written_over_the_image_it_is_made_from` fails on Linux, and clippy 1.99 reports
  `single_element_loop` in `dc2tk-assets` (hence `--no-deps` and `-p`).
- `./dc2tk engine verify dc2.iso` after touching `ps2.json` or the copy step.
- `python3 -I tools/test_engine_mods.py` and `python3 -I tools/test_native_prepare.py`: engine-mod
  and native preparation, without Docker or a disc.
- `python3 -I port/tests/run.py`: the port's checks (`--iso dc2.iso` adds the retail movies).
- `python3 -I tools/port.py --movies` after touching `port.json`, `port/` or `tools/port.py`, then
  a run to the costume screen and a run into the castle dungeon, checked by screenshot.
- `tools/golden.sh` for anything that can reach what dc2tk writes (development.md).

## The shape

One engine source, two outputs. ChronicleTwo's decompiled engine is a git submodule. For a set of
mods dc2tk prepares one patched copy of it, which MWCC compiles into the `SCES_511.90` that goes
on the ISO and clang compiles into a native binary. Both read the same disc image, so
scripts, assets, maps, data tables and engine changes from one `dc2tk build` run the same in
PCSX2, on a PS2 and natively.

## Decisions

- **The engine stays upstream.** ChronicleTwo (MIT) is a submodule at `engine/`, pinned to a
  commit, with its own Kazarov submodule. No decompiled code is committed here and nobody edits
  the checkout: builds change a copy under `build/`. Changes to its build scripts, and the
  token-sized substitutions clang and the 64-bit port need in its game source, live here as
  patches in `tools/engine-patches/`; no decompiled function is ever copied here, and what the
  port or a mod needs different beyond that is done by replacing functions on the copy.
- **Releases carry no engine.** Neither engine source nor a native binary: users build the port
  with dc2tk, clang and SDL3, as they build an edited executable with dc2tk and Docker. A release
  zip has `engine.commit`, `tools/engine-patches/`, the Python helpers and `port/`.
- **The port is minimal and runs the game as the PS2 did** (see Rules). ChronicleTwo's own port
  (519 overridden game functions, Luau, a converted `data.zip`) is not used.
- **Mods can change engine code**, and the change works in the ISO build and in the port.
- **The port is native, not emulated.** The game's 149 units compile with token substitutions.
  Where an engine function drove PS2 hardware (GS packets, VU1 programs, the IOP sound modules),
  the port replaces that function with one written fresh that draws through SDL_GPU or calls the
  synth. The rest of the engine runs as it is.
- **Saves** are a folder per memory card. **Movies** decode with libavcodec, linked dynamically.
- Engine builds for the ISO need Docker on every OS (Docker Desktop on Windows); the port builds
  on macOS and Linux (Windows later) with `dc2tk build --native`, which drives clang itself,
  without CMake.

## Part 1: the engine in dc2tk

### The submodule

`engine/` is TheMoonPeople/ChronicleTwo at a pinned commit; `git submodule update --init
--recursive` also brings `engine/tools/kazarov`. It carries MWCC and MWLD
(`engine/tools/compilers/mw/`), the matching build and the Dockerfile of the image the build runs
in. Bumping the pin is how upstream fixes arrive; after a bump, `engine verify` must pass and every
patch must still apply.

`dc2tk engine fetch` initialises the submodule in a clone of this repository; where there is none
(a release zip), it clones ChronicleTwo at the commit in `engine.commit` and its Kazarov
submodule, with git.

### Building the executable

The PS2 build (`engine/ps2/CMakeLists.txt`) needs:

- MWCC 3.0-011126 and MWLD 2.4-001213, Windows programs run under wibo;
- Satan's Fiddle (Adubbz/SatansFiddle at `365415f` with four patches from
  `engine/tools/kazarov/image/patches/`), which runs MWCC under LLDB 19 to set its internal state;
  there is no prebuilt binary, and the build refuses to run MWCC without it;
- splat64 0.50.0, binutils-mips-ps2-decompals v0.10, and the Python in `engine/scripts/build/`
  (about 2k lines of object fixups);
- one input from the disc: the retail `SCES_511.90` (SHA-256 `41dec168…`).

All 149 game units are C++ and none is split assembly; the CRT, MW runtime, libc, libm, libgcc
and the 33 Sony SDK units are assembled from the split retail executable. dc2tk does not port any
of this to Rust. `dc2tk engine build` copies the engine to `build/engine/<copy>/` (all of it but
`.git`, `port/`, `docs/` and the like, `ps2.json` applied, the retail executable put in
`rom/pal/extracted/iso/`), asks Kazarov's driver for its image (`kazarov image`, built the first
time), and runs `kazarov --inside build ps2-pal <target>` in it with the copy mounted at
`/work/chronicletwo`. The result is `build/pal/SCES_511.90` and its link map `.xMAP` in the copy.

`dc2tk engine verify` builds the `build` target, which runs ChronicleTwo's `verify.py` against
retail section by section; dc2tk then compares the files too, which differ (the link adds sections
nothing loads).

### A build mode for edited engines

| Hazard | Where | State |
|---|---|---|
| Absolute symbols: `CHA_DEV_FONT_PIECE_*` and `D_1F64A00` (crt0's BSS end and heap start) | `engine/ps2/config/pal/SCES_511.90.lcf:1474-1481` | `ps2.json`: the font pieces dropped (libdev's object defines them), `D_1F64A00 = __bss_end` |
| Every retail-named data section gets alignment 1 and is padded to retail | `engine/scripts/build/postprocess_object.py:1791-1792` | `ps2.json`: the compiler's alignment is kept |
| The default target verifies against retail | `engine/ps2/CMakeLists.txt:263-268` | `engine build` builds the `elf` target |
| `_stack` is fixed at `0x01F80000`; the C library's heap is the 110 KB between retail's BSS end and it | `SCES_511.90.lcf:1452-1455` | every engine build reports the gap and fails when BSS reaches the stack |
| A Satan's Fiddle profile row whose function changed is a compile error | `engine/scripts/build/satansfiddle.json` | the rows of every unit a mod changes are dropped (fix 4: not for table-only units) |
| External vtables must equal retail; dng_main has checks of its own | `postprocess_object.py:460-477, 619-641` | `edited-units.txt` lists the changed units; no edit has reached these checks yet |
| crt0 clears BSS from `StopCamera__9mgCCamera`, retail's first BSS symbol | the split `crt0.s` | new BSS placed before that symbol would not be cleared; watch for it |

### Putting the executable on the disc

`rebuild::replace_iso_file` (`src/crates/dc2tk-disc/src/rebuild.rs`) writes a root file in place
when it fits its sectors and otherwise past the end of the volume, rewriting its directory record
and the volume size (not the UDF entry, which the PS2 does not read). `dc2tk build` uses it for
an engine-mod build's executable and for `--executable FILE`.

## Engine changes in mods

As built; the user guide is [docs/guide/mods.md](docs/guide/mods.md#engine-files).

- A mod's `engine/<unit>.cpp` extends that unit of `engine/ps2/src`; a new name adds a unit.
- A function marked `PORT_OVERRIDE` replaces the engine's function of the same name and
  signature; an unmarked one is new. Kazarov's `port_override.py` (MIT, plain Python) does the
  stripping, run in the container.
- `original(...)` inside a replacement calls the engine's own version, kept on the copy under a
  new name; not for constructors, destructors, operators or nested and namespace-qualified
  classes.
- A whole header at its `engine/ps2/include/` path replaces that header.
- `dc2tk mod override engine <mod> <function>` copies the engine's function into the mod, marked;
  it writes to the user's `mods/`, never this repository.
- Two mods replacing one function or one header clash, naming both.
- Each mod set has a copy, `build/engine/mods-<hash>/`, the hash over each mod's name, files and
  bytes. `build --mod` builds it (also with `--dry-run`) and puts its executable on the image;
  `mod check --disc` builds it too.
- **Executable tables:** with no engine mod, table edits stay byte patches on retail's executable
  and need no Docker. With one, `engine_tables.rs` writes the edited values into the copy's
  initializers (every table but `data:parts`, fix 5), and refuses when a mod's engine code also
  changes an initializer a JSON edit reaches. The port always builds tables from source.
- **Natives:** the native tables and the SDK declarations scripts compile against are regenerated
  from the prepared copy (`engine_natives.rs`, under `build/mod-sdk/include/` in the copy), so a
  mod can add a native and call it from its scripts. Both targets compile the same table.

What engine code in a mod keeps to: C++ that MWCC 3.0 accepts; no inline asm, hardware register
addresses or fixed RAM addresses; only `sce*` functions the port implements; no function
`port/src` replaces; room below the PS2 stack.

## Part 2: the native port

### The boundary

The engine draws by building GS packets and VU1 programs and sending them by DMA, and it plays
sound through IOP modules over RPC. The port replaces the engine functions that do that, about 90
of them marked `PORT_OVERRIDE` in `port/src`, with functions that do the same work natively. The
game reads its files with `sceCdRead` from the ISO it is given, so whatever dc2tk builds runs on
it.

- **2D and register state:** `mgCDrawPrim` (`mg_drawprim.cpp`), `CPreSprite` (`prespr.cpp`) and
  the mglib frame functions (`mglib.cpp`) write GS registers into a state machine
  (`draw.cpp`): PRIM, RGBAQ, UV, XYZ, TEX0/1, CLAMP, ALPHA, TEST, ZBUF, FRAME, SCISSOR, FOGCOL,
  TEXA and the BITBLT registers. A vertex kick becomes a `render_draw` with the state the GS would
  have used.
- **Blending:** the GS's (A-B)*C+D maps to SDL_GPU's blend factors, with source alpha, destination
  alpha and the FIX constant; alpha 0x80 is 1.0, as on the GS.
- **Textures:** RAM TIM2 and IM2 images are decoded once (4- and 8-bit CLUTs with CSM1, page-
  swizzled 8-bit, 16/24/32-bit) and cached by image and palette with a checksum checked once a
  frame. A texture the game also draws into (its VRAM address is a frame buffer) is the same GPU
  texture, so render-to-texture effects work. CLUT copies go to a palette table, never to the
  retail RAM palette.
- **Frame buffers:** a target per GS frame base, at least 640x512, with one depth buffer shared
  per size. `mgEndFrame` presents the display target. Depth reads (`sceGsExecStoreImage` of
  PSMZ32/24) are served by a depth-to-colour pass.
- **Models:** `mgCVisualMDT` faces become GPU meshes once (`mg_visual.cpp`,
  `visualmotion.cpp`), drawn by `mesh.vert`, which does what `Vu_prog0` did: the game's own
  model-screen matrix, three directional and four point lights, fog, and skinning from a 32-bone
  palette. Sorted draws keep the game's `mgCDrawManager` order.
- **The other VU1 programs**, written fresh in C++ from `engine/docs/vu1/`: 3D sprites
  (`mg_sprite.cpp`, `prog_3dsp`), shadow volumes (`mg_shadow.cpp`, `prog_sdw`) and water
  (`water.cpp`, `prog_wtr`).
- **Sound:** `CSound` (`sound.cpp`) and `CSound::StreamOpenState` (`ezbgm.cpp`) call `midi.c`
  (the retail HD/BD/SQ banks and sequences, port layout, voice allocation), `audio.c` (BGM
  streams, movie PCM, SDL output) and `spu2.c` (48 voices with ADSR, Gaussian interpolation and
  the nine reverb presets). There is no IOP, RPC, SIF DMA or IOP heap. An audit of 841 retail
  banks and 435 sequences found no unsupported feature.
- **Movies:** `CMovie` is replaced; the port demultiplexes the PSS file, libavcodec decodes the
  MPEG-2 video, and the frame is shown in place of the display, as the PS2 showed it.

### How it is built

`dc2tk build extracted/scripts dc2.iso --native [FILE] [--movies]`, or `tools/port.py`, which
does the same and is kept in step with `native.rs` (fix 7):

- copies `engine/ps2` with `port.json` applied (173 token-sized substitutions: clang fixes,
  declarations of guarded-out asm, pointer widths, native sizes and strides, renames of a retail
  function to `*Retail` where an override must call it), strips the functions `port/src`
  replaces, then replays the chosen mods;
- compiles the 149 units at `-O0`, ChronicleTwo's four asm files (weakened), Kazarov's
  `libvu0.cpp`, and `port/src`;
- renames the engine's `operator new` and `delete` to the port's arena and its `main` to
  `port_game_main`, aliases the four names the engine spells differently, and links with SDL3
  (and libavcodec with `--movies`). Nothing is left unresolved.

### Choices

- **64-bit, with the game's addresses in a 32-bit window.** Retail data on the disc and in
  memory holds 32-bit pointers, and the game masks addresses with `0x0FFFFFFF`. On macOS the
  binary places its data, BSS and a 768 MB zero-fill segment at `0x100000000` and above,
  re-executes itself once with ASLR off so they land there, and mirrors the window at `+0x20000000`
  and `+0x30000000` for the uncached segments; the scratchpad is a 16 KB segment of its own.
  Linux uses a memfd at low addresses. Struct fields that held a PS2 pointer are
  `ps2_pointer<T>`. Removing the window would mean changing every loader that fixes up pointers
  in place, so it stays.
- **Upstream's `PORT` macro and headers**, with `-DPORT -DPAL`; `port.h` undoes the NTSC timing
  `PORT` alone selects.
- **The MWCC asm**: 54 functions are guarded out under `PORT`. 44 come from ChronicleTwo's four
  files; the port writes mg_frame's 7, `ClipBoxXZ`, `PlaneNormalXZ` and `SendDMA`.
- **Shaders** are GLSL, compiled ahead of time to SPIR-V and Metal source by
  `tools/port_shaders.py` (`spirv-cross --msl-decoration-binding`, or Metal buffer slots follow
  use order).
- **Engine bugs a 64-bit build exposes** are fixed by substitutions as found, for example
  `S51Thunder`'s two-float `ratio` that `GetTimeLightingRatio` fills with four, and
  `MenuCommonReadData`'s 4-byte pointer stride. ChronicleTwo's `engine/port/src` and its probe
  scripts in `engine/scripts/port` are a good place to look for more of them.

### Modules

| Files | What | Lines |
|---|---|---|
| `main.c`, `memory.c`, `kernel.c`, `runtime.c` | start, the address window, the engine's arena, EE threads as fibers, semaphores, VBlank, MW runtime | 630 |
| `cdvd.c`, `executable.c`, `split_data.c`, `devices.c` | the ISO, `cdrom0:` files, the executable's symbols, split data, SIF devices | 590 |
| `render.c`, `render.h`, `port/shaders` | SDL_GPU: targets, textures, pipelines, batching, present | 1,040 |
| `draw.cpp`, `mg_drawprim.cpp`, `mglib.cpp`, `prespr.cpp`, `packets.c`, `graphics.c`, `dma.cpp`, `present.c` | the GS state machine, the texture cache, 2D drawing, frame and display | 1,460 |
| `mg_visual.cpp`, `visualmotion.cpp`, `mg_sprite.cpp`, `mg_shadow.cpp`, `water.cpp` | models, 3D sprites, shadows, water | 920 |
| `audio.c`, `midi.c`, `spu2.c`, `sound.cpp`, `ezbgm.cpp`, `iop.c` | the sound library and the `CSound` replacements | 1,560 |
| `input.c`, `memory_card.c`, `movie.cpp`, `movie_decode.c` | pads, cards, movies | 1,000 |
| `frame.cpp`, `collision.cpp`, `link_fixups.cpp` | asm replacements | 230 |
| `port/debug/*` (debug build only), `hooks.c`, `port_stats.h` | debug window and API, counters, scenario controls, frame capture, input replay, savestates; the normal build has only empty hooks and counters | |

About 7,900 lines with headers and shaders, not counting the generated `shaders.h`; the tests
are about 3,100 more.

## Milestones

Each has a gate. A gate that needs PCSX2, a PS2, a display or a listener is the user's.

1. **E1 Submodule.** Done.
2. **E2 Modding build mode.** Built. Gate, the user's: the 50,000-money edit boots in PCSX2.
3. **E3 Engine mods for the ISO.** Built. Gate, the user's: a mod replacing a dungeon function
   plays in PCSX2. Fixes 3, 4, 13 first.
4. **E4 Tables and natives.** Built but `data:parts` (fix 5) and the table byte check (fix 4).
   Gate, the user's: a shop edit and a mod's native work in PCSX2.
5. **P0 Compile spike.** Done.
6. **P1 Headless boot.** Done: the ISO boots to the language selector.
7. **P2 Graphics.** Built natively, 50 frames a second in a window on macOS. Gate, the user's:
   compared with PCSX2. Next: see water, lens flare, depth of field and texture animation on
   screen; run on Linux; then Windows.
8. **P3 Input.** Built. Gate: New Game, Palm Brinks, the first dungeon floor, played by a person.
   Next: replay from `first-floor-selection-save` past the gate where it crashed, and melee hits.
9. **P4 Sound.** Built. Gate: effects, music and voices, heard against retail.
10. **P5 Saves and movies.** Built. Gate: a save made natively loads again (passed by replay); the
    opening movie plays (passed by replay); a save from a PS2 loads.
11. **P6 Engine mods natively.** Built. Gate: one mod with an engine change, a native called from
    a script, a table edit and a texture plays the same in PCSX2 and natively. Fixes 3, 6, 7.
