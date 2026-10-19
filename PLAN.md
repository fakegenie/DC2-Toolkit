# Plan

The engine in dc2tk, mods that change it, and a minimal native Linux port. The facts about
ChronicleTwo come from its source at commit `d51e74b` (Kazarov at `14164f1`), given as paths under
the submodule, `engine/...`.

## Picking this up

### Where things stand

| Milestone | State |
|---|---|
| E1 the engine as a submodule, built and verified | done |
| E2 a build that an edited engine relinks in | built; the PCSX2 boot is the user's to try |
| E3 engine mods for the ISO | next: tasks below |
| E4 executable tables and natives from source | after E3 |
| P0 compile spike | done; `tools/port.py` reproduces it |
| P1 headless boot | next: tasks below |
| P2 to P6 | later, in order |

The E and P tracks are independent until P6. Work on the branch `engine`; never push, merge or
change `main` without the user.

### Read first

- This file, then [src/docs/development.md](src/docs/development.md) (dc2tk's rule and checks)
  and [src/README.md](src/README.md) (the crates).
- `src/crates/dc2tk/src/engine.rs`: `dc2tk engine build/verify/fetch`, the copy step and the patch
  substitutions.
- `tools/engine-patches/ps2.json` and `port.json`: every change made to the engine's files, as
  exact text substitutions.
- `tools/port.py`: the port's build until P6 moves it into dc2tk.
- ChronicleTwo's `engine/docs/vu1/*.md`, `engine/docs/SOUND_DAT.md`, `engine/docs/MWCC.md` and
  `engine/docs/SATANSFIDDLE.md` when a task reaches them.

### Rules

- **No decompiled code in this repository.** The engine stays in the submodule, and nobody edits
  `engine/`. Every build works on a copy under `build/`. What has to change in the engine's files
  is a substitution in `tools/engine-patches/*.json`, and only token-sized ones in game source
  (`void` to `int`, one declaration, one literal), never a function body. A larger change is a
  function replaced on the copy. Test mods written for checks use bodies of our own, never a copy
  of a decompiled one.
- **dc2tk's rule:** a disc image built with nothing edited is retail's, byte for byte
  (development.md). With no engine change, the disc keeps retail's `SCES_511.90`. After any change
  to the PS2 patches, `dc2tk engine verify` must still say every section is retail's.
- **The port runs the game as the PS2 did:** retail formats read from the ISO, the original `.stb`
  scripts on the original VM, PAL timing at 50 Hz, the GS's own resolution. No 60 fps, no
  converted data, no replaced textures, no other scripting language, nothing from ChronicleTwo's
  `port/` but the four asm files `tools/port.py` names. Kazarov's `src/cpp/sce` (MIT) may be read
  for how a library behaves.
- **Code:** functions and plain data; no classes, wrappers or higher-order helpers in code of our
  own. No comments, except what the conventions need to work: the one-line `//!` module doc and
  clap's `///` help text in Rust, and a usage docstring at the top of a Python tool. The port is
  C (C11) except where an engine type makes C++ necessary.
- **Prose** (docs, commit messages) is plain and direct, in the style of the existing docs and
  `git log`.
- **Ask the user** before installing anything system-wide (SDL3 for P2) and before anything that
  needs sudo.

### This machine

- Linux x86-64 with clang 21, binutils, Python 3 and Docker. No Rust, cmake, ninja, pkg-config or
  SDL3. No disc image and no PCSX2: gates that need either are the user's to run.
- **Rust runs in Docker.** The image `dc2tk-rust` (rust:1-trixie with rustfmt and clippy) is
  built. Run cargo as:

  ```
  docker run --rm --user $(id -u):$(id -g) -e CARGO_HOME=/cargo -e CARGO_INCREMENTAL=0 \
      -v "$HOME/.cache/dc2tk-cargo:/cargo" -v "$PWD:$PWD" -w "$PWD/src" dc2tk-rust cargo build --release
  ```

  from the repository root (`mkdir -p ~/.cache/dc2tk-cargo` first). The binary lands in
  `build/release/dc2tk`, which `./dc2tk` runs.
- **Kazarov's image** `kazarov-dev:01b70a4a6008` is built; `dc2tk engine` finds it by itself.
- **The retail executable** (no ISO) is at
  `/home/dev/projects/decomp-bench/artifacts/chronicletwo/rom/pal/extracted/iso/SCES_511.90`
  (SHA-256 `41dec168…`, as ChronicleTwo expects). `dc2tk engine build/verify` take it in place of
  a disc.
- **Times:** `engine verify` from nothing takes about 5 minutes and an unchanged rebuild under a
  second; one edited unit relinks in about 14 s. `tools/port.py` builds the port in about 6 s.

### Checks

- `cargo fmt --all`, then `cargo clippy --release -p <crate> --no-deps --all-targets -- -D warnings`
  for each crate changed, then `cargo test --release -p <crate>`.
- Two failures predate this work and are not yours to fix: the `dc2tk-disc` test
  `a_disc_is_not_written_over_the_image_it_is_made_from` fails on Linux, and clippy 1.99 reports
  `single_element_loop` in `dc2tk-assets` (hence `--no-deps` and `-p`).
- `./dc2tk engine verify <SCES_511.90>` after touching `ps2.json` or the copy step.
- `python3 -I tools/port.py` after touching `port.json`, `port/` or `tools/port.py`; `--run`
  runs the result.

### Without a disc image

E3 and E4 can be built and checked here: build the engine, then read the link map
(`build/engine/<copy>/build/pal/SCES_511.90.xMAP`) and disassemble the changed function
(`llvm-objdump-21 -d --triple=mipsel-unknown-linux-gnu --start-address=… --stop-address=…`), as
E2's test edit was checked. P1 can be built up to the first file the game reads; past that it
needs the ISO, which the user can put at `dc2.iso` in the repository root (`*.iso` is ignored).

## The shape

One engine source, two outputs. ChronicleTwo's decompiled engine is a git submodule. For a set of
mods dc2tk prepares one patched copy of it, which MWCC compiles into the `SCES_511.90` that goes
on the ISO and clang compiles into a native Linux binary. Both read the same disc image, so
scripts, assets, maps, data tables and engine changes from one `dc2tk build` run the same in
PCSX2, on a PS2 and natively.

## Decisions

- **The engine stays upstream.** ChronicleTwo (MIT) is a submodule at `engine/`, pinned to a
  commit, with its own Kazarov submodule. No decompiled code is committed here and nobody edits
  the checkout: builds change a copy under `build/`. Changes to its build scripts, and the few
  token-sized substitutions clang needs in its game source, live here as patches in
  `tools/engine-patches/`; no decompiled function is ever copied here, and what the port or a mod
  needs different beyond that is done by replacing functions on the copy.
- **Releases carry no engine.** Neither engine source nor a native binary: users build the port
  with dc2tk, clang and SDL3, as they build an edited executable with dc2tk and Docker. A release
  zip has `engine.commit` and `tools/engine-patches/`.
- **The port is minimal and runs the game as the PS2 did** (see Rules). ChronicleTwo's own port
  (519 overridden game functions, Luau, a converted `data.zip`) is not used.
- **Mods can change engine code**, and the change works in the ISO build and in the port.
- **The port is emulated at the SDK and IOP boundary.** The game's 149 units compile unchanged
  and call the `sce*` functions they call on a PS2; the port supplies those functions and the
  hardware behind them.
- **Saves** are a folder per memory card. **Movies** decode with libavcodec, linked dynamically.
- Engine builds for the ISO need Docker on every OS (Docker Desktop on Windows); the port is Linux
  x86-64 only. `tools/port.py` builds the port until P6, when dc2tk drives clang itself, without
  CMake.

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

Upstream has only the matching build. An edited engine meets these:

| Hazard | Where | State |
|---|---|---|
| Absolute symbols: `CHA_DEV_FONT_PIECE_*` and `D_1F64A00` (crt0's BSS end and heap start) | `engine/ps2/config/pal/SCES_511.90.lcf:1474-1481` | done in `ps2.json`: the font pieces dropped (libdev's object defines them), `D_1F64A00 = __bss_end` |
| Every retail-named data section gets alignment 1 and is padded to retail; an object whose size changed is skipped silently | `engine/scripts/build/postprocess_object.py:1791-1792` | done in `ps2.json`: the compiler's alignment is kept |
| The default target verifies against retail | `engine/ps2/CMakeLists.txt:263-268` | done: `engine build` builds the `elf` target |
| `_stack` is fixed at `0x01F80000`; retail BSS ends at `0x01F64A00`, and the C library's heap is the 110 KB between | `SCES_511.90.lcf:1452-1455` | kept; E3 task 9 reports the gap |
| A Satan's Fiddle profile row whose function changed is a compile error | `engine/scripts/build/satansfiddle.json` | E3 task 5 |
| External vtables must equal retail; dng_main has checks of its own | `postprocess_object.py:460-477, 619-641` | E3 task 6, when an edit reaches them |
| crt0 clears BSS from `StopCamera__9mgCCamera`, retail's first BSS symbol | the split `crt0.s` | new BSS placed before that symbol would not be cleared; watch for it |

### Putting the executable on the disc

Done: `rebuild::replace_iso_file` (`src/crates/dc2tk-disc/src/rebuild.rs`) writes a root file in
place when it fits its sectors and otherwise past the end of the volume, rewriting its directory
record and the volume size (not the UDF entry, which the PS2 does not read).
`dc2tk build ... --executable FILE` uses it to put an engine-built executable on an image, and
refuses when the mods also patch tables inside the executable (E4).

## Engine changes in mods

### In a mod folder

```
mods/my-mod/
  mod.json
  engine/
    userdata.cpp      functions for the engine's userdata.cpp
```

- A file in `engine/` is named after a unit of `engine/ps2/src`, and its functions go into that
  unit.
- A function marked `PORT_OVERRIDE` replaces the engine's function of the same name and
  signature, as in ChronicleTwo's port: `PORT_OVERRIDE int CUserDataManager::AddMoney(int amount)
  { ... }`. A function without the mark is new.
- Two mods replacing one function clash, as script patches do.
- Later in E3: `original()` to call the engine's own version, a header a mod replaces, a new unit,
  and `dc2tk mod override engine`.

Kazarov's `engine/tools/kazarov/scripts/build/port_override.py` (MIT, plain Python, no libclang)
does the replacing: `list DIR -o FILE` writes the names a folder's marked definitions link under,
and `strip --overrides FILE [--private-functions] [--private-variables] SRC OUT` writes a unit
without the definitions those names replace.

### One prepared copy, two compilers

For a mod set dc2tk prepares `build/engine/mods-<hash>/` (ISO) and the port's copy:

1. copy the engine with the patches;
2. apply the mods' engine files;
3. write the edited executable tables as source (E4);
4. regenerate the native tables from the copy (E4).

Then:

- **ISO:** MWCC in the container, link, and the executable onto the image.
- **Port:** clang on the host (`port.json` applied, then the mods), linked with `port/`.

At P6, `dc2tk build extracted/scripts dc2.iso -o modded.iso --native darkchronicle --mod my-mod`
writes both. The binary records the SHA-256 of the executable built beside it; at start the port
reads `SCES_511.90` from the ISO it is given and warns when it differs. A stock binary records
retail's hash.

### Tables inside the executable

Mods that edit the monster table, spectrumising, Donny's shop, the character tables, gifts,
Spheda, breeding and parts of the inventions and fishing tables get byte patches on
`SCES_511.90` today (`src/crates/dc2tk-assets/src/elf.rs`, `exe_tables.rs`). The port never runs
that file. When the engine is built from source, dc2tk writes each edited table as a replacement
definition of its global (`dony_shoplist`, `etcitem_spectol_table`, ...;
`engine/ps2/src/menushop.cpp:243`, `gamedata.cpp:17`) with the JSON's values, and both compilers
build it: one C writer per table `exe_tables.rs`, `monster_data.rs`, `invention_data.rs` and
`fishing_data.rs` patch.

ISO builds with no engine changes keep the byte patches and need no Docker. Tables read from disc
files (items, weapons, shop lists) need nothing: the port reads them from the ISO.

### What engine code in a mod keeps to

`dc2tk mod check` compiles a mod's engine files with MWCC and, from P6, with clang, and checks
that it:

- is C++ that MWCC 3.0 accepts: nothing from C++11 on;
- has no inline MIPS or VU0 asm, no hardware register addresses and no fixed RAM addresses, and
  reaches everything by symbol;
- calls only `sce*` functions the port implements (the port's link names any others);
- replaces no function the port itself replaces;
- leaves room below the PS2 stack.

### Natives

The native tables (`event_func.cpp`, `runscript_opcodes.cpp`, `actscript.cpp`, `effscript.cpp`)
come from the prepared copy, so a mod can add a native in its engine code and call it from its own
scripts through the regenerated SDK headers. Both targets compile the same table.

## Part 2: the native Linux port

### The boundary

The port is the PS2's software and hardware below the game: the Sony SDK libraries the retail
executable links as assembly, the IOP modules it loads, and the DMA, VIF, VU1, GS and SPU2 they
drive. The game reads its files with `sceCdRead` from the ISO it is given, so whatever dc2tk
builds runs on it.

### What is there

- `tools/engine-patches/port.json`: 18 substitutions on the engine's copy (below).
- `port/include/port.h`: the header forced into every engine unit. It defines the `PORT_*`
  macros, reads `types.h` with `u_long` as 64 bits, turns `PORT_RECORD_ASSERT` into
  `static_assert`, and keeps the PAL values of `PAL_TIMING`.
- `port/src/link_fixups.cpp`: `CameraCtrlParam::operator=`, which the engine declares and never
  defines.
- `tools/port.py`: copies `engine/ps2` to `build/port/copy/` with `port.json` applied; compiles
  the 149 units, ChronicleTwo's four asm files (`engine/port/src/mg_math.cpp`, `collision_asm.cpp`,
  `gameutil_asm.cpp`, `mg_drawprim_asm.cpp`, their symbols made weak so the engine's own
  definitions win) and every `port/src/*.c` and `*.cpp`, only what changed; makes the two
  duplicate symbols local and aliases the four the engine names differently; links
  `build/port/darkchronicle` non-PIE, with every symbol nothing defines yet as a stub that prints
  its name and aborts; and lists those in `build/port/missing.txt` by library.

Today: 192 stubs (libvu0 26, eekernel 22, libmc 17, sifrpc/sifcmd/IOP heap 15, libcdvd 14,
libvifpk 12, libgraph 11, libpad 11, libgifpk 10, the engine's own asm 10, libmpeg 9, sifdev 8,
split data 7, libdma 6, libdev 5, MW runtime 4, modmsin 3, libsdr 2), and the binary runs until
`unimplemented: sceVu0UnitMatrix`, called from a static constructor. Each P1 task removes stubs;
a function is done when it leaves `missing.txt`.

### Choices

- **x86-64.** `u_long128` is `unsigned __int128`, which clang has only on 64-bit targets, and the
  PS2's `long` is 64 bits as Linux x86-64's is.
- **Every address the game sees lies below 256 MB, as on the PS2.** The game makes DMA addresses
  with `& 0x0FFFFFFF` (mg_visual 131 and 739, mg_shadow 259, mg_sprite 526, movie), so a pointer
  above `0x10000000` breaks even where nothing truncates it. Linked non-PIE, the whole image from
  `.text` to `_end` is about 35 MB at `0x400000`, and the game's heap is `main_buffer`, a 26 MB
  `.bss` array in mainloop.o. Fiber stacks and the few host-heap allocations the game makes
  (`new CEffectCtrl`, effect.cpp:1088) must be low too. Code that keeps pointers in `u32` and
  `int` fields (189 implicit narrowings, 115 explicit `(uptr)` casts) then works unchanged.
- **The uncached segments exist.** mg_visual (689, 703, 720, 764) and mg_shadow (239) write
  packets through `pointer | 0x20000000`, and DMA tags carry `0x30000000` addresses. Before the
  static constructors run, the binary's `.data` and `.bss` move into a memfd mapped at their own
  addresses and again at `+0x20000000` and `+0x30000000`. The scratchpad is mapped at
  `0x70000000`.
- **Upstream's `PORT` macro and headers.** A guard of our own would mean editing upstream files.
  The copy compiles with `-DPORT -DPAL` and ChronicleTwo's MWCC-tolerance flags (in
  `tools/port.py`). `PORT` alone switches the 95 `PAL_TIMING` sites to NTSC values, which
  `port.h` undoes. `PORT` changes no disc record's layout: its 52 `PORT_RECORD_ASSERT`s hold on
  x86-64, and `uptr` fields appear only in structs that never touch the disc.
- **The MWCC asm**: 54 functions are guarded out under `PORT`. 44 come from ChronicleTwo's four
  files. The other 10 are the port's own C: mg_frame's 7 (`mgCFrame::GetBBoardMatrix`,
  `GetDrawRect`, `GetLWMatrix`, `GetLWMatrixTopBottom`, `mgInsideScreen`, `test1`, `test2`;
  upstream's are tied to its glTF renderer), `ClipBoxXZ`, `PlaneNormalXZ`, and `SendDMA`, which
  starts the DMA emulation. Each is written from the engine's asm (`engine/ps2/src`, inside
  `#ifndef PORT`) and what calls it.
- **`port.json`** holds what clang needs: `virtual void Draw()` to `int` in `dng_event.hpp` and
  `dng_hud.hpp` and their two definitions (MWCC gave them a new vtable slot; clang refuses the
  override); an explicit `operator=` for `CFuncPoint` in `mapload.hpp`; block-scope anonymous
  unions holding non-trivial types in gyorace, menucapt, menucls1 and sceneevent made storage and a
  reference; MWCC's varargs builtins in dbg_font made `va_list`; the 128-bit literal in mg_visual
  (twice); and declarations of the guarded-out functions whose callers remain, in collision,
  editmap2, gameutil, mg_frame and mg_math.
- **Split data comes from the disc.** The VU1 microcode (`Vu_progmain`, `Vu_prog0`,
  `Vu_prog_sdw`, `Vu_prog_3dsp`, `Vu_prog_wtr`), the first DMA chain (`My_dma_start0`) and `_gp`
  have no source. The port reads the first six from the `SCES_511.90` on the ISO it runs, by its
  symbol table, into the stub storage `tools/port.py` makes for them (to be made the right size).
- **Functions the port replaces** are replaced on the copy by the same step mods use. The known
  case is the movie code, which writes IPU DMA registers directly (`movie.cpp:226-227, 766-918`).
- **VU1 is an interpreter** running the microprograms the game uploads itself. ChronicleTwo's
  `engine/docs/vu1/` (3,000 lines of specs) is what it is checked against. Hand-written C for
  prog0 only if profiling asks.
- **The GS is a software rasteriser** over a 4 MB local memory array: render to texture, CLUTs,
  `sceGsStoreImage` readback and the water's frame-buffer reads come with it, and SDL3 only shows
  a texture. If speed falls short, rasterising in bands on several threads comes first.
- **Sound is emulated at the RPC boundary**, with no IOP processor. The modules are LIBSD and
  SDRDRV, MODMIDI and MODHSYN (Sony's HD/BD/SQ synthesiser and sequencer), EZMIDI and EZBGM
  (`engine/ps2/src/main.cpp:103-130`). The EE side of each protocol is in `sound.cpp`,
  `ezmidi.cpp` and `ezbgm.cpp`; the formats are already worked out in `dc2tk-assets`
  (`soundbank.rs`, `music.rs`) and [docs/internals/](docs/internals/README.md). SPU2 reverb comes
  after the voices.
- **Movies are skipped at first**: the movie ends as it starts. At P5 they decode with
  libavcodec's MPEG-2 decoder, linked dynamically (LGPL), the port's only dependency besides
  SDL3; the PSS demultiplexing and the sound beside the video are the port's own.

### Modules

Line counts are estimates.

| File | What | Lines |
|---|---|---|
| `main.c` | SDL3, the game's `main` on a fiber, 50 Hz pacing | 300 |
| `memory.c`, `kernel.c` | Low memory, mirrors, scratchpad, EE threads as ucontext fibers, semaphores, VBlank handlers | 800 |
| `cdvd.c` | ISO 9660, `cdrom0:` paths, `sceCdRead`, `sceOpen` | 400 |
| `sce/*.c` | libdma, libgraph, libvifpk, libgifpk, libvu0, libpad (SDL gamepad and keyboard, DualShock 2 analog), libmc (a folder per card) | 1,500 |
| `dma.c`, `vif.c` | DMA chains; VIF1 UNPACK, MSCAL, MSCNT, DIRECT | 700 |
| `vu1.c` | VU1 interpreter | 2,000 |
| `gs.c`, `present.c` | GS registers, swizzled memory, rasteriser, blending and tests, the displayed buffer to SDL | 4,000 |
| `spu2.c`, `iop_sound.c` | 48 ADPCM voices with ADSR; the sdrdrv, CSL and ez* handlers | 2,500 |

## Milestones

Each has a gate. A gate that needs the ISO or PCSX2 is the user's to run when the work is built.

1. **E1 Submodule.** Done 2026-10-19: `dc2tk engine verify` finds every section from `.text` to
   `.bss` retail's.
2. **E2 Modding build mode.** Built 2026-10-19 (the hazard table). A test edit (new games start
   with 50,000 money, through two calls that grow `CUserDataManager::Initialize`) relinked with
   4,857 `.text` symbols moved; the growth fitted the slack before the next section, so no data
   moved. Gate, the user's: that edit boots in PCSX2 with `build --executable`.
3. **E3 Engine mods for the ISO.** Gate: a mod replacing a dungeon function plays in PCSX2 (the
   user's); here, the link map and disassembly show the replacement, and `engine verify` still
   passes.
   1. Find each chosen mod's `engine/*.cpp`. A file not named after a unit of `engine/ps2/src` is
      an error naming the mod and the file. Unit-test it with a mods folder in a temporary
      directory.
   2. Give each mod set a copy of its own: `build/engine/mods-<hash>/`, the hash the first 12 hex
      digits of a SHA-256 over each engine file's mod name, path and bytes, in order. Reuse
      `engine.rs`'s `prepare` with `ps2.json`.
   3. Run Python in the container: a function beside `run_in_container` that runs
      `python3 <args>` in Kazarov's image with the copy mounted, for `port_override.py`.
   4. For each unit some mod changes: `port_override.py list` over a folder holding that mod's
      file for the unit, to `build/mod-overrides/<unit>.<mod>.txt` in the copy. If two mods list
      a name, stop and name both mods and the function. Then `strip --overrides <all of them
      joined> --private-functions --private-variables ps2/src/<unit>.cpp ps2/src/<unit>.cpp`, and
      append `\n#define PORT_OVERRIDE\n` and each mod's file, in mod order, to the unit.
   5. Remove from the copy's `scripts/build/satansfiddle.json` every row whose `translation_unit`
      (or `name`, in the `translation_units` list) is a unit a mod changes. Those units no longer
      match retail, and a row whose function changed stops the compile.
   6. Write the changed units' names to `edited-units.txt` at the copy's root. If a POST step
      raises for one of them (vtables, dng_main), add a substitution to `ps2.json` that skips that
      check for units listed there, then run `engine verify` to show the unedited build is
      unchanged.
   7. `dc2tk build --mod X`, when a chosen mod has engine files: build the mod set's copy (target
      `elf`), and put its `SCES_511.90` on the image with `replace_iso_file`, as `--executable`
      does; refuse when the mods also patch executable tables (E4) and when `--executable` is
      given. `--dry-run` builds the engine too, as the check of a mod's engine code.
   8. `dc2tk mod check` builds the engine copy when the mod has engine files.
   9. After an engine build, read `__bss_end` from the link map and print what is left between it
      and the stack at `0x01F80000` (the stack is the 512 KB above): the C library's heap, about
      110 KB in retail. Fail when it is negative, and warn when it is less than retail's.
   10. Docs: a section on engine files in `docs/guide/mods.md`, and the commands in
       `docs/guide/commands.md`.
   11. Then, each with its own check: `original()` (keep the engine's definition renamed
       `<name>__original` instead of stripping it, and `#define original <name>__original` around
       the mod's function; a member function's renamed declaration goes into the copied class
       header), a header a mod replaces (whole file; two mods on one header clash), a new unit
       (its object added to `main_o_files` and the linker script's lists), and
       `dc2tk mod override engine <mod> <function>` (the function's text from the copy into the
       mod, marked `PORT_OVERRIDE`; it lands in the user's `mods/`, never in this repository).
4. **E4 Tables and natives.** Executable tables written as source, natives from the prepared copy.
   Gate: a shop edit and a native a mod adds work on the ISO.
5. **P0 Compile spike.** Done 2026-10-19; `tools/port.py` is its result.
6. **P1 Headless boot.** Gate: with the ISO, `python3 -I tools/port.py --run dc2.iso` logs the
   files the title screen loads. Each task's check: the binary runs past the stubs the task
   removes, and `missing.txt` no longer lists them.
   1. libvu0: add Kazarov's `engine/tools/kazarov/src/cpp/sce/libvu0.cpp` (MIT) to
      `tools/port.py`'s upstream list if it compiles on its own; otherwise write the 26 functions
      in `port/src/vu0.c` from it.
   2. `port/src/memory.c`, in a `__attribute__((constructor(101)))` that runs before the
      engine's static constructors: move the binary's `.data` and `.bss` (from `__data_start` or
      the first writable page to `_end`, page-aligned) into a memfd mapped at their own addresses
      and at `+0x20000000` and `+0x30000000`; map 16 KB at `0x70000000`; `mallopt(M_MMAP_MAX, 0)`
      so every host allocation comes from the brk heap, which sits low after `.bss`. A check
      writes through `pointer | 0x20000000` and reads the plain pointer.
   3. The MW runtime: `mwInit` (nothing to do), `fptosi`, `fptoui` and `__construct_new_array`,
      each written in C from its split assembly in the E1 copy
      (`build/engine/pal/ps2/asm/pal/lib/mwcc/*.s`).
   4. `port/src/kernel.c`: the eekernel functions in `missing.txt` (threads as ucontext fibers on
      stacks that are `.bss` arrays, so they are low; semaphores; INTC and DMAC handlers kept in a
      table and called by the VBlank; `FlushCache`, `DIntr` and `EIntr` doing nothing; `Exit__2`
      exiting). `main` runs the game's `main` on a fiber. Kazarov's
      `src/cpp/sce/eekernel_threads.cpp` shows the semantics.
   5. `port/src/cdvd.c`: the disc image named on the command line, read with ISO 9660; libcdvd's
      `sceCdInit`, `sceCdSearchFile`, `sceCdRead` (synchronous) and `sceCdSync`, the `sceCdSt*`
      streaming reporting nothing for now; sifdev's `sceOpen`, `sceRead`, `sceLseek`, `sceClose`
      on `cdrom0:\PATH;1` names. Every file opened is logged with its size: the gate's log.
   6. The IOP: `sceSifLoadModule` and the reboot succeeding, `sceSifBindRpc` giving a bound
      client, `sceSifCallRpc` answering with zeros, `sceSdRemote` doing nothing, until P4.
   7. Graphics without a screen: libvifpk and libgifpk's packet builders written for real (they
      only write memory), `sceDmaSend` and `sceGsSyncPath` doing nothing, `sceGsSyncV` waiting
      out 1/50 s and running the VBlank handlers. libgraph's setup functions record what they are
      given.
   8. libpad reporting a DualShock 2 with nothing pressed; libmc reporting no card; libdev and
      modmsin doing nothing; libmpeg reporting the movie at its end.
   9. The split data: from the ISO's `SCES_511.90`, the six symbols' bytes by its symbol table
      into storage of their size (change `tools/port.py`'s stub size to fit them).
7. **P2 Graphics.** Gate: the title screen in a window, compared with PCSX2. Ask the user to
   install SDL3's development package first. In order: the DMA chain walker behind `sceDmaSend`
   and `SendDMA`; VIF1 (UNPACK, STCYCL, MSCAL, MSCNT, DIRECT); the VU1 interpreter, checked
   against `engine/docs/vu1/`; the GS (registers, the swizzled 4 MB memory, the rasteriser,
   blending and tests); presenting the displayed buffer in an SDL3 window, scaled to its aspect.
8. **P3 Input.** Gate: New Game, Palm Brinks, the first dungeon floor.
9. **P4 Sound.** Gate: effects, music and voices.
10. **P5 Saves and movies.** Gate: a save made natively loads again; the opening movie plays.
11. **P6 Engine mods natively.** `--native` in `dc2tk build` (the port's build moved from
    `tools/port.py` into dc2tk, from the same copy), the executable hash check, `mod check` with
    clang. Gate: one mod with an engine change, a native called from a script, a table edit and a
    texture plays the same in PCSX2 and natively.
