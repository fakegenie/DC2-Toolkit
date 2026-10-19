# Plan

The engine in dc2tk, mods that change it, and a minimal native Linux port. Nothing here is built
yet; the facts about ChronicleTwo come from reading its source at commit `d51e74b` (Kazarov at
`14164f1`), and paths into it are given as they will be under the submodule, `engine/...`.

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
  token-sized substitutions clang needs in its game source (P0 found 11), live here as patches in
  `tools/engine-patches/`; no decompiled function is ever copied here, and what the port or a mod
  needs different beyond that is done by replacing functions on the copy.
- **Releases carry no engine.** Neither engine source nor a native binary: users build the port
  with dc2tk, clang and SDL3, as they build an edited executable with dc2tk and Docker.
- **The port is minimal and runs the game as the PS2 did.** It reads the retail formats from the
  ISO, runs the original `.stb` scripts on the original VM, at PAL 50 Hz and the GS's own
  resolution. No 60 fps, high resolutions, replaced textures, converted data or other scripting
  language. ChronicleTwo's own port (519 overridden game functions, Luau, a converted `data.zip`)
  is not used.
- **Mods can change engine code**, and the change works in the ISO build and in the port.
- **The port is emulated at the SDK and IOP boundary.** The game's 149 units compile unchanged
  and call the `sce*` functions they call on a PS2; the port supplies those functions and the
  hardware behind them.
- **Saves** are a folder per memory card. **Movies** decode with libavcodec, linked dynamically.
- Engine builds for the ISO need Docker on every OS (Docker Desktop on Windows); the port is Linux
  x86-64 only; dc2tk drives clang for it directly, without CMake.

## Part 1: the engine in dc2tk

### The submodule

`engine/` is TheMoonPeople/ChronicleTwo at a pinned commit; `git submodule update --init
--recursive` also brings `engine/tools/kazarov`. It carries MWCC and MWLD
(`engine/tools/compilers/mw/`), the matching build and the Docker image the build runs in.
Bumping the pin is how upstream fixes arrive.

A release zip has no submodule. dc2tk is compiled with the pinned commit (read from the gitlink),
and `dc2tk engine fetch` downloads that commit's archive from GitHub into `engine/` where there
is no git checkout. The README says the engine comes from ChronicleTwo, which hosts it.

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
of this to Rust: `dc2tk engine build` runs ChronicleTwo's build in Kazarov's Docker image
(linux/amd64), on a prepared copy, and takes back the ELF and MWLD's link map. Docker is needed
only for builds whose mods change engine code.

dc2tk's rule holds: with no engine changes the retail executable goes on the disc untouched.
`dc2tk engine verify` builds the unedited copy and runs ChronicleTwo's `verify.py`, which compares
the built code and data with retail byte for byte.

### A build mode for edited engines

Upstream has only the matching build. An edited engine meets these:

| Hazard | Where | In the modding mode |
|---|---|---|
| Absolute symbols: `CHA_DEV_FONT_PIECE_*` point into libdev `.rodata`, which moves when code grows; `D_1F64A00` is retail's end of BSS | `engine/ps2/config/pal/SCES_511.90.lcf:1474-1481` | Derive from section symbols; find what reads `D_1F64A00` |
| `_stack` is fixed at `0x01F80000`; retail BSS ends at `0x01F64A00`, about 110 KB below | `SCES_511.90.lcf:1452` | Kept; `build` reports what a mod leaves of the gap |
| Every data section gets alignment 1 and is padded to retail; an object whose size changed is skipped silently | `engine/scripts/build/postprocess_object.py:1510-1512, 1791-1792` | Edited units keep the compiler's alignment |
| A Satan's Fiddle profile row whose function changed is a compile error | `engine/scripts/build/satansfiddle.json` | Rows of replaced functions are dropped |
| External vtables must equal retail; dng_main has checks of its own | `postprocess_object.py:460-477, 619-641` | Off |
| The default target verifies against retail | `engine/ps2/CMakeLists.txt:263-268` | The `elf` target |

These are build scripts, not game code. dc2tk keeps the mode as patches in `tools/engine-patches/`
and applies them to its copy; when the pin moves they are applied again, and one that no longer
applies stops the build, naming it.

### Putting the executable on the disc

`rebuild::overwrite_iso_file` (`src/crates/dc2tk-disc/src/rebuild.rs:293`) refuses a file whose
size changed. A built executable that outgrows its sectors moves past the end of the volume:
`iso::set_extent` already rewrites the directory record, and the volume size and the UDF file
entry follow as `engine/scripts/build/make_iso.py:311-381` does it.

## Engine changes in mods

### In a mod folder

```
mods/my-mod/
  mod.json
  engine/
    dng_main.cpp      functions replacing dng_main's, and new ones
    my_feature.cpp    a new unit
    my_feature.hpp
```

- A function in `engine/<unit>.cpp` replaces the engine's function of the same qualified name and
  signature; any other function is new. A file named after no unit is a new unit.
- `original()` calls the engine's own version, as in script patches: in the copy the engine's
  definition is renamed `Foo__original`, the mod's body is wrapped in
  `#define original Foo__original`, and a member function's renamed declaration is added to the
  copied class header.
- `dc2tk mod override engine my-mod DngMainKey` copies a function into the mod, as `mod override`
  does for scripts.
- Two mods replacing one function clash, as with scripts.
- A mod's `engine/<header>.hpp` replaces that header in the copy, and two mods changing one header
  clash. `mod check` warns when a struct's layout changes: every unit including it recompiles,
  and disc records and saves may no longer fit it.

Stripping and renaming definitions in C++ uses Kazarov's `port_override.py` (MIT, libclang), run
in the same container.

### One prepared copy, two compilers

For a mod set dc2tk prepares `build/engine/<mod set hash>/`:

1. copy `engine/ps2/`;
2. apply the mods' engine changes;
3. write the edited executable tables as source (below);
4. regenerate the native tables from the copy.

Then:

- **ISO:** MWCC in the container in the modding mode, link, and the ELF onto the image.
- **Port:** clang on the host. The port's own few replacements are applied first, as a base under
  the mods, and the result links with `port/`.

`dc2tk build extracted/scripts dc2.iso -o modded.iso --native darkchronicle --mod my-mod` writes
both. The binary records the SHA-256 of the executable built beside it; at start the port reads
`SCES_511.90` from the ISO it is given and warns when it differs. The image carries nothing
extra, and a stock binary records retail's hash.

### Tables inside the executable

Mods that edit the monster table, spectrumising, Donny's shop, the character tables, gifts,
Spheda, breeding and parts of the inventions and fishing tables get byte patches on
`SCES_511.90` today (`src/crates/dc2tk-assets/src/elf.rs`, `exe_tables.rs`). The port never runs
that file. When the engine is built from source, dc2tk writes each edited table as a replacement
definition of its global (`dony_shoplist`, `etcitem_spectol_table`, ...; `engine/ps2/src/menushop.cpp:243`,
`gamedata.cpp:17`) with the JSON's values, and both compilers build it. That is one C writer per
table `exe_tables.rs`, `monster_data.rs`, `invention_data.rs` and `fishing_data.rs` patch.

ISO builds with no engine changes keep the byte patches and need no Docker. Tables read from disc
files (items, weapons, shop lists) need nothing: the port reads them from the ISO.

### What engine code in a mod keeps to

`dc2tk mod check` compiles a mod's engine files with MWCC and with clang and checks that it:

- is C++ that MWCC 3.0 accepts: nothing from C++11 on;
- has no inline MIPS or VU0 asm, no hardware register addresses and no fixed RAM addresses, and
  reaches everything by symbol;
- calls only `sce*` functions the port implements (the port's link names any others);
- replaces no function the port itself replaces;
- leaves room below the PS2 stack: `build` checks the gap even when it makes only the native
  binary, so nothing works natively and fails on the console.

### Natives

The native tables (`event_func.cpp`, `runscript_opcodes.cpp`, `actscript.cpp`, `effscript.cpp`)
come from the prepared copy, so a mod can add a native in its engine code and call it from its own
scripts through the regenerated SDK headers. Both targets compile the same table.

## Part 2: the native Linux port

### The boundary

The port is the PS2's software and hardware below the game: the Sony SDK libraries the retail
executable links as assembly, the IOP modules it loads, and the DMA, VIF, VU1, GS and SPU2 they
drive. The game reads its files with `sceCdRead` from the ISO it is given, so whatever dc2tk
builds runs on it. Port code is C99 in this repository's style (functions and plain structs); the
engine stays the C++ it was decompiled as.

### Choices

- **x86-64.** `u_long128` is `unsigned __int128`, which clang has only on 64-bit targets, and the
  PS2's `long` is 64 bits as Linux x86-64's is.
- **Every address the game sees lies below 256 MB, as on the PS2.** The game makes DMA addresses
  with `& 0x0FFFFFFF` (mg_visual 131 and 739, mg_shadow 259, mg_sprite 526, movie), so a pointer
  above `0x10000000` breaks even where nothing truncates it. Linked non-PIE, the whole image from
  `.text` to `_end` is about 35 MB at `0x400000`, and the game's heap is `main_buffer`, a 26 MB
  `.bss` array in mainloop.o. Fiber stacks and the few host-heap allocations the game makes
  (`new CEffectCtrl`, effect.cpp:1088) go to a low arena too. Code that keeps pointers in `u32`
  and `int` fields (189 implicit narrowings, 115 explicit `(uptr)` casts) then works unchanged.
  (ChronicleTwo put its arenas above 4 GB so that such stores crash, and rewrote the loaders.)
- **The uncached segments exist.** mg_visual (689, 703, 720, 764) and mg_shadow (239) write
  packets through `pointer | 0x20000000`, and DMA tags carry `0x30000000` addresses. At start,
  before the static constructors run, the game's `.data` and `.bss` move into a memfd mapped at
  their own addresses and again at `+0x20000000` and `+0x30000000`. The scratchpad is mapped at
  `0x70000000`.
- **Upstream's `PORT` macro and headers.** A guard of our own would mean editing upstream files.
  The copy compiles with `-DPORT -DPAL` and ChronicleTwo's MWCC-tolerance flags: `-fms-extensions
  -fno-strict-aliasing -fwrapv -ffp-contract=off -fno-strict-return -ftrivial-auto-var-init=zero
  -Wno-c++11-narrowing -Wno-register -Wno-return-mismatch`. `PORT` alone switches the 95
  `PAL_TIMING` sites to NTSC values, so the port's forced header redefines `PAL_TIMING(pal, ntsc)`
  as `pal`. `PORT` changes no disc record's layout: its 52 `PORT_RECORD_ASSERT`s hold on x86-64,
  and `uptr` fields appear only in structs that never touch the disc.
- **The MWCC asm**: 54 functions are guarded out under `PORT` (mg_math 33, mg_frame 7, gameutil
  4, mg_drawprim 4, collision 3, editcoll 1, editmap2 1, mg_visual 1). 44 come from upstream's MIT
  files, which compile on their own: `engine/port/src/mg_math.cpp`, `collision_asm.cpp`,
  `gameutil_asm.cpp`, `mg_drawprim_asm.cpp`, with `vu0_lanes.hpp`. The other 10 are the port's
  own C: mg_frame's 7 (upstream's version is tied to its glTF renderer), `ClipBoxXZ`,
  `PlaneNormalXZ`, and `SendDMA`, which starts the DMA emulation.
- **What the copy needs to compile with clang.** 141 of 149 units compile as they are. The rest
  need small text substitutions, carried as patches:
  - headers: `virtual void Draw()` to `int` in `dng_event.hpp:337` and `dng_hud.hpp:556` (MWCC
    gave them a new vtable slot; clang refuses the override), and an explicit `operator=` for
    `CFuncPoint` in `mapload.hpp` (a union member deletes the implicit one);
  - units: the two matching `Draw` definitions; block-scope anonymous unions holding non-trivial
    types in gyorace, menucapt, menucls1 and sceneevent; MWCC's varargs builtins in dbg_font; a
    128-bit literal in mg_visual;
  - link: `MainBuffer` (`extern` then `static` in mainloop) and the second `VectMatMul` in
    eventedit made local; `BuffWorkData__2` and `EventCamera__2` aliased to retail's duplicates;
    the `PrimQuad<float>`/`<int>` instantiations aliased to `PrimQuad_f_`/`_i_`;
    `CameraCtrlParam::operator=` defaulted.
- **Split data comes from the disc.** The VU1 microcode (`Vu_progmain`, `Vu_prog0`,
  `Vu_prog_sdw`, `Vu_prog_3dsp`, `Vu_prog_wtr`) and the first DMA chain (`My_dma_start0`) have no
  source. The port reads them from the `SCES_511.90` on the ISO it runs, by its symbol table.
- **Functions the port replaces** are replaced on the copy by the same step mods use. The known
  case is the movie code, which writes IPU DMA registers directly (`movie.cpp:226-227, 766-918`).
- **VU1 is an interpreter** running the six microprograms the game uploads itself (progmain,
  prog0, prog_sdw, prog_3dsp, prog_wtr, dma_start0). ChronicleTwo's `engine/docs/vu1/` (3,000
  lines of specs) is what it is checked against. Hand-written C for prog0 only if profiling asks.
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
- **dc2tk drives clang** for the port, as it drives the container for the executable, with a
  content-hash cache of objects so an unchanged unit is not compiled twice. All 149 units compile
  in about 6 s at `-O0` on 12 cores. SDL3 comes from the system, through pkg-config.

### What the port supplies

Linked against the host's libc, libm and C++ runtime, the 149 units leave 241 symbols undefined:

- the Sony SDK and kernel, 172 functions: libvu0 26, eekernel 23 (threads, semaphores, interrupt
  handlers, `FlushCache`), libmc 17, sifrpc/sifcmd and the IOP heap 15, libcdvd 14 (with the
  `sceCdSt*` streaming), libvifpk 12, libgraph 11, libpad 11, libgifpk 10, libmpeg 9, sifdev 8,
  libdma 6, libdev 5, modmsin 3, libsdr 2; Kazarov's `libvu0.cpp` (MIT) covers libvu0;
- the 54 MWCC asm functions above;
- the MW runtime: `mwInit`, `fptosi`, `fptoui`, `__construct_new_array`;
- the 6 split data symbols above, and the aliases and `operator=` above.

### Modules

Line counts are estimates.

| File | What | Lines |
|---|---|---|
| `main.c` | SDL3, the game's `main` on a fiber, 50 Hz pacing | 300 |
| `memory.c`, `kernel.c` | Low arena, scratchpad, EE threads as ucontext fibers, semaphores, VBlank handlers | 800 |
| `cdvd.c` | ISO 9660, `cdrom0:` paths, `sceCdRead`, `sceOpen` | 400 |
| `sce/*.c` | libdma, libgraph, libvifpk, libgifpk, libvu0, libpad (SDL gamepad and keyboard, DualShock 2 analog), libmc (a folder per card) | 1,500 |
| `dma.c`, `vif.c` | DMA chains; VIF1 UNPACK, MSCAL, MSCNT, DIRECT | 700 |
| `vu1.c` | VU1 interpreter | 2,000 |
| `gs.c`, `present.c` | GS registers, swizzled memory, rasteriser, blending and tests, the displayed buffer to SDL | 4,000 |
| `spu2.c`, `iop_sound.c` | 48 ADPCM voices with ADSR; the sdrdrv, CSL and ez* handlers | 2,500 |

About 12k lines, against ChronicleTwo's port at about 68k plus 24k in Kazarov and 36k of Rust.

## Milestones

Each has a gate that has to pass before the next starts. E1 comes first, as it brings the source
both tracks need; after it the E and P tracks are independent until P6.

1. **E1 Submodule.** `engine/` at a pinned commit, `dc2tk engine fetch`, `dc2tk engine build` in
   the container. Gate: `dc2tk engine verify` matches retail. Done 2026-10-19 at ChronicleTwo
   `d51e74b` (Kazarov `14164f1`): every section from `.text` to `.bss` is retail's. The file is
   not (it differs from byte `0x20`, 4,072,148 bytes against 4,054,516: the link adds sections
   nothing loads), so with no engine changes the disc keeps retail's file. The first build, with
   the split, took 5 minutes; an unchanged rebuild takes under a second. Making the image the
   first time took a few minutes more.
2. **E2 Modding build mode.** The hazards above, the executable moved on the disc when it grows.
   Gate: a one-line engine change boots in PCSX2. Built 2026-10-19, the boot is still to try:
   `tools/engine-patches/ps2.json` drops the font-piece absolutes (libdev's object defines them),
   makes `D_1F64A00` (crt0's BSS end and heap start) `__bss_end`, and keeps the compiler's
   alignment for retail-named data sections; the unedited build with them is still retail's in
   every section. `--engine DIR` builds an edited checkout in a copy of its own, and
   `build --executable` puts the result on an image, past the end of the volume when it grew. A
   test edit (new games start with 50,000 money, through two calls that grow
   `CUserDataManager::Initialize`) relinks in 14 s with 4,857 `.text` symbols moved; the growth
   fitted the slack before the next section, so no data moved. Left for E3, where replaced
   functions are known: dropping their Satan's Fiddle rows, and the vtable and dng_main checks
   when an edit reaches them. crt0 clears BSS from `StopCamera__9mgCCamera`, retail's first BSS
   symbol, so new BSS placed before it would not be cleared.
3. **E3 Engine mods for the ISO.** Replacements, `original()`, new units, headers, `mod override
   engine`, `mod check` with MWCC. Gate: a mod replacing a dungeon function plays in PCSX2.
4. **E4 Tables and natives.** Executable tables written as source, natives from the prepared copy.
   Gate: a shop edit and a native a mod adds work on the ISO.
5. **P0 Compile spike.** Every unit compiled with clang under `PORT`, every missing symbol an
   abort stub that prints its name. Gate: the list of missing functions, which replaces the
   estimates above. Done 2026-10-19: all 149 units link into one non-PIE binary with the
   substitutions above, which runs until its first stub (`mgUnitMatrix`, from a static
   constructor); the findings are in "Choices" and "What the port supplies".
6. **P1 Headless boot.** Memory, fibers, cdvd, VSync. Gate: the log shows the title's files
   loading from the ISO.
7. **P2 Graphics.** DMA, VIF, VU1, GS, presenting. Gate: the title screen in a window, compared
   with PCSX2.
8. **P3 Input.** Gate: New Game, Palm Brinks, the first dungeon floor.
9. **P4 Sound.** Gate: effects, music and voices.
10. **P5 Saves and movies.** Gate: a save made natively loads again; the opening movie plays.
11. **P6 Engine mods natively.** `--native` from the prepared copy, the executable hash check,
    `mod check` with clang. Gate: one mod with an engine change, a native called from a script,
    a table edit and a texture plays the same in PCSX2 and natively.
