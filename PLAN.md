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
Linux x86-64 builds and plays too (2026-10-21, in a Docker container on the Mac; see "This
machine"): both binaries link with 0 stubs, the title, a town and a dungeon play headless on
Vulkan, and `port/tests/run.py` passes; fixes 17 to 21 are what it left. Part 2 below
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

- **Play, through a replay harness** (since removed for the debug build's recordings; controller
  reports injected at the input boundary, SDL's dummy video and audio drivers): the castle tutorial and its soldiers;
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
9. **Done: debugging in the shipping binary.** `DC2TK_SCREENSHOT` and `DC2TK_PRESS` are gone (the
   debug API's `screenshot` and `press` replace them); the sound dump is the debug build's
   (`debug_audio`, `DC2TK_SOUND_DUMP`); `cdvd.c` logs opened files and archive members, and loads
   the archive index for that, only after `port_cdvd_log_files(1)`, which the debug build calls.
10. **Done:** `dma_chain.c` is deleted with the rest of the emulation.
11. **Done:** `port/tests/run.py` writes its patched sources and objects under `build/port/tests/`,
    and `-DPORT_SOUND` and its conditionals are gone. Every unit keeps `-I engine/port/src`:
    `mg_drawprim.cpp` and others include `vu0_lanes.hpp` from there.
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
17. **Savestates on Linux.** Done with the redesign (savestate entry 2 below): states keep the
    `game_data`/`game_bss` sections, and a non-PIE binary never needs relocation.
18. **`profile` on Linux.** `port/debug/core.cpp` runs macOS's `/usr/bin/sample`; use `perf`, or
    say it is missing.
19. **Golden baselines are per GPU.** Lavapipe's pictures differ from Metal's by 1 to 3 per
    channel on about half the title's pixels, and up to 44 on seven edge pixels; the `capture`
    draw lists at the same frame are identical, so the CPU side matches. Keep baselines per
    platform (for example `build/port/golden/baseline-<platform>/`), or compare across GPUs
    with a tolerance and an outlier count.
20. **The default save folder** on Linux is `./saves` (macOS uses iCloud Drive when it exists).
    Use `$XDG_DATA_HOME/Dark Chronicle/saves`, falling back to `~/.local/share`.
21. **The costume screen draws white patches over Max** (face, hands, a block over the body),
    on macOS and Linux alike, in the release build after New Game.
22. **Done: debug warps from the title during the attract demo.** The warp's Start+Select press
    only ended the demo; a warp from the title now presses again every 30 frames until the title
    is left (`port/debug/scenario.cpp`).
23. **Done: the blue screen while fishing.** Aiming a cast sets the camera's near and far distance
    equal, and `CCameraControl`'s height ratio divided 0 by 0. The PS2 FPU gives no NaN there; the
    host did, so the camera went NaN, only the sky drew and frames took a second. `port.json`
    guards the division. The same can hide anywhere the engine divides by a range: a solid sky
    picture, an empty depth buffer or a NaN camera in a recording's trace points at it.
24. **The fishing recording.** `memory_tour.py record fishing` sets Max at the Palm Brinks canal
    with a rod and bait 317 (Carrots draw nothing there) and records the user's catch; the user
    still has to record it (the old input file did not survive the move to recordings). Then add
    starts for Finny Frenzy, Spheda, photos and Georama.

### Next: debugging and states that last

What the fishing work kept needing, in the order to build it:

1. **Engine variables by name.** Done: `peek name=CastDist [type=f32|i32|u8|ptr|str|hex] [count=N]`,
   `poke`, `symbols match=` and `watch` (a Watch tab in the debug window) read and write any global
   or file static through the executable's own symbol table. `status` also reports the player's
   and the camera's position.
2. **Savestates on every platform.** Done. A state loads only in the build that made it (the
   header carries a build id hashed from every linked object), and that build can load it in any
   later launch on macOS, Linux and iOS. What keeps it simple:
   - States are taken only at the end of a main loop frame (`port_end_main_frame`), with every
     other EE thread parked in `GamePadStep` and no interrupt running, so no stack is saved and no
     return address has to survive. Nested loops that call `mgEndFrame` themselves (the START
     pause, event movies, now-loading, the language select) never reach it; a request waits, and
     the debug API gives up after 300 frames with a message.
   - Everything the game owns sits in one place: `tools/port.py` renames engine objects' data and
     BSS to segment `__GAME` (Mach-O) or sections `game_data`/`game_bss` (ELF), and port files put
     their game variables between `PORT_GAME_BEGIN`/`PORT_GAME_END` (`port/include/port_game.h`).
     The state is that range, found through the linker's section bounds, plus audio and the mesh
     cache. Host handles (file descriptors, GPU objects, fibers) stay outside it.
   - A launch at another address (iOS always, macOS with `DC2TK_ASLR=1`) moves every 8-byte word
     that points into the old image by the slide: into code and constants only when 4-aligned,
     into the arena only inside a used block, plus the live main-thread frames. In a dungeon
     state that is 117,000 words; 35 lookalikes in one buffer and 14,800 stale pointers into dead
     stack frames are moved or left without effect. Linux links without PIE and macOS starts the
     same address unless ASLR is asked for, so they never move anything.
   Checked: town and dungeon states load in a new process (macOS, ASLR macOS, iOS simulator
   across launches, Linux) with identical pictures 120 frames later; a state from another build is
   refused.
3. **Recordings.** Done (`port/debug/recording.cpp`, the Recordings tab). One feature records play
   so it can be replayed to debug or test a change, in another process or another build:
   - The debug build records from launch all the time. F8, `recording_start` or "Record from
     here" takes a checkpoint instead and records from it: the save data (`CSaveData`), the loop,
     map or floor, the player's position and rotation, the random seed and the seed the current
     dungeon floor was laid out with (`port_floor_seed`, patched into `RandomMapMainProc`). The
     checkpoint is loaded straight away through the game's own warp, so recording and playback
     start from the same load. It is refused in menus, events, minigames (holding the fishing rod is one), map changes and nested loops.
   - A recording is that start, every frame's input, the debug commands that change the game
     (scenario and reload commands, `poke`, mod commands), the scheduler tick each field fell on,
     a per-frame trace of player and camera, and a copy of the memory card. F9, `recording_save`
     or Save writes it to `build/port/recordings/NAME` (a name with a `/` is a folder anywhere); a
     crash writes `crash-<time>`, the fault watch `fault-<time>`. Loading a savestate ends it.
   - `recording_play`, Play, or `DC2TK_RECORDING=folder` at launch plays one: a checkpoint
     recording loads its checkpoint (from boot if need be), a launch one restarts the game with
     `DC2TK_RECORDING`. Playback makes fields fall on the recorded ticks (`port_field_script` in
     `graphics.c`), so play recorded at full speed replays exactly at any speed: the field count
     during loads is what wall-clock timing changed. It pauses at the end (or `pause=N`) and
     reports how far player and camera strayed from the trace, writing `play.jsonl`.
   Checked: town and dungeon checkpoints, a normal-speed recording through a map change, launch
   recordings, a crash recording, each replayed in a new process and after other play within 0.001.
   Before the field script, a load at normal speed took 3 fields where an unpaced one took 11.
4. **Fault watch.** Done (`port/debug/fault.cpp`, `faults`, Recordings tab). Every frame the player,
   the camera position and the camera, view and projection matrices are checked for NaN, infinity
   or a position past a million; the first bad frame logs a `fault:` line and saves the recording
   as `fault-<time>` (`faults pause=1` or `DC2TK_FAULT_PAUSE=1` also pauses there; memory_tour
   fails on a fault). The camera is not checked while a menu is up: `MenuWorldTrans` reads the menu
   camera's position before its first step, so the first menu frame always has a leftover camera,
   on the PS2 too. A pause by default looked like a freeze in play. The places tour (330 places)
   raised none.
5. **The PS2's own `rand`.** The engine's `rand` was the host C library's, whose state no savestate holds,
   so every replay that drew random numbers after a load strayed (a dungeon floor loaded from a
   state came out different). `port/src/runtime.c` now has the PS2's own newlib `rand`
   (next * 1103515245 + 12345, low 31 bits, starting at 1) and its seed sits in the game data states keep.
   Random numbers now follow the PS2 sequence, so input recorded before this may play differently.

### Next: rendering speed (branch `render-speed`)

The game ticks 25 times a second (PAL: `mgFrameRate` 2 at 50 fields) and must keep doing so;
everything above that is shown frames. Measured 2026-10-25 on the recordings `perf-swamp` (Fish
Monster Swamp) and `perf-palm_brinks`: a tick costs 3-5 ms on the Mac and ~9 ms on an iPhone 14 Pro
Max, but a swamp frame is 600-1800 draws in 55-81 render passes (Palm Brinks 30-38). Character
outlines (`COutLineDraw::Draw`, two target switches per character) and texture animation
(`mgCTextureAnime::TexAnime`) split the frame; each split reloads and stores the whole target (4x
samples with MSAA), which a tile GPU pays for in bandwidth. Between frames come only from
`smooth_until` inside the game's field waits, on the game thread, and redraw every recorded pass:
one fits per tick on the iPhone (50 shown a second at 120 Hz), and in the swamp `present_ms` reaches
12-24 ms.

0. **Measure.** Done: `play.jsonl` timings, `gpu_us` in stats (submit to fence, on a timer thread)
   and `tools/bench.py` over `port/tests/recordings`. Mac (M-series, background window, scale 1):
   unpaced 346 ticks/s in the swamp and 395 in Palm Brinks, game 2.4 ms and GPU 2-2.8 ms a tick,
   so the Mac can already draw ~350 frames a second. Paced with `--smooth 0` it shows ~55 a second
   on a 98 Hz display: `smooth_until` only starts a between frame that ends 5 ms (half a refresh)
   before the field, game logic eats the start of a field, and a between frame can block 10 ms in
   `nextDrawable`. With smoothing the real frame is drawn offscreen and then replayed at alpha 0,
   so a tick costs 5-6.5 ms of GPU instead of 2-3.
1. **Outlines inside the main pass.** Done: `port/src/outline.cpp` overrides `COutLineDraw::Draw`.
   The character's direct draws are held, drawn four times shifted by the edge width with the
   edge colour as a tint (normal blend, no depth write, no shadow), then once more as the body
   composited at the character's alpha. `MeshUniforms.edge` names the side so smoothing pairs
   each copy with the same side a tick before. Swamp unpaced with 4x MSAA: 34 to 11 render passes
   a frame, GPU 3.0 to 2.0 ms, 303 to 421 ticks a second; Palm Brinks 29.5 to 11.2 passes.
2. **No mid-frame splits.** Texture animation, shadows and depth of field before the main pass;
   never reload an MSAA target; resolve once.
3. **A render thread.** The game thread hands over each finished frame (passes, draws, transforms);
   the render thread draws at the display's rate, blending the last two, and neither waits on the
   other. Screen readbacks (`MenuCapture`, `mgStoreImage`) have to meet it.
4. **Less per tick.** Dirty marks instead of `texture_checksum` every frame; fewer, bigger draws.
5. **60 ticks (optional, later).** Menus already run at `mgFrameRate` 1 and movement scales by it,
   but timers, animations and events count ticks.

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
- **Test aids** (fix 9): the debug build writes the mixed sound to `DC2TK_SOUND_DUMP=file.raw`
  (48 kHz stereo 16-bit); screenshots and presses go through the debug API. `debug.py launch`
  starts the game muted and behind other windows (`--sound`, `--front` undo that), as the golden,
  sweep and benchmark tools do.
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
  `capture`, `toggle name=shadows value=0`, `recording_start`, `recording_save name=...`,
  `recording_play name=...`, `save slot=1`, `load slot=1`. Check the game through `screenshot` and `windowshot`, never a full-screen capture.
- **Golden screenshot tests:** `python3 -I tools/golden_port.py` runs `port/tests/golden/*.json`
  against the debug build, paused and stepped frame by frame; `--update` writes the baselines to
  `build/port/golden/` (game imagery, never in git).
- **Crash sweep:** `python3 -I tools/event_sweep.py` plays every row of the debug menu's event list
  (`event/view_pal.txt`) in its own copy of the debug build (port 7791, throwaway saves) and writes
  `build/port/event-sweep.json` with the top of each crash's backtrace; `--rows 32,129` or `--from 145`
  for a subset. Rows run in order without a save reset, as the in-game list does, so a crash can need
  the rows before it. It sets `DC2TK_MEMORY_STRICT=1`, so a pool overflow during an event is a crash too.
- **Memory pool tour:** `python3 -I tools/memory_tour.py places|menus|models|recordings|untested|all`
  runs its own debug build (port 7793, throwaway saves, `DC2TK_MEMORY_STRICT=1` so the first overflow
  stops the game) from the cap8 preset with `unlock`, and visits every map and floor, opens every menu
  through the debug API's `menu`, equips every weapon, outfit and ridepod part, transforms into every
  monster form, and plays every recording in `port/tests/recordings` and `build/port/recordings` in a
  fresh game (failing on one that strays from its trace). `untested` lists the pool
  set-up sites in the source that no tour has reached. Results and screenshots go to
  `build/port/memory-tour`. `memory_tour.py record fishing` (a built-in start) or `record NAME town
  map=m03` sets the game up, records from a checkpoint there and saves the recording to
  `port/tests/recordings/NAME` when Enter is pressed. While paused, `press` holds
  input only for frames that are stepped. Debug commands added for it: `menu open=NAME`, `seed`,
  `water` (fishing water around the player), `equip ... bait=N`, and `fishing` in `status`.
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
- **Linux** (the machine of the earlier work): clang 21, binutils, Python 3 and Docker; Rust in
  the `dc2tk-rust` image (`printf 'FROM rust:1-trixie\nRUN rustup component add rustfmt clippy\n' |
  docker build -t dc2tk-rust -`); `python3 -I tools/port_sdk.py` copies SDL3 out of Kazarov's
  image and `python3 -I tools/movie_sdk.py` builds FFmpeg 8.0.1's MPEG-2 decoder.
- **Linux x86-64 from this Mac:** OrbStack's Docker runs `--platform linux/amd64` containers
  under Rosetta on a real Linux kernel. A Debian trixie image with `clang lld llvm binutils
  python3 pkg-config libsdl3-dev libavcodec-dev libavutil-dev mesa-vulkan-drivers` (clang 19,
  SDL 3.2.10, FFmpeg 7.1) is enough; `tools/port.py` then finds SDL3 and FFmpeg through
  `pkg-config`, and `--movies --debug` links both binaries in about a minute. Add `xvfb` for an
  X display, or run with `SDL_VIDEO_DRIVER=offscreen` (Vulkan on lavapipe works with either);
  `SDL_AUDIO_DRIVER=disk SDL_AUDIO_DISK_OUTPUT_FILE=out.raw` records the sound. Mount the
  repository at its own path, mount a separate folder over `build/port` so the Linux and macOS
  builds do not overwrite each other, and mount `dc2.iso` too. Speed there (8 to 50 frames a
  second on lavapipe under Rosetta) says nothing about real hardware.
- **What Linux does differently:** EE threads switch with `ucontext` (about 47 switches a
  second, 0.3 µs each, so no asm is needed); the binary is non-PIE at `0x400000`, so it loads at the
  same address every launch and savestates never move a pointer; nothing is mapped at PS2
  addresses; shaders take the SPIR-V
  path (`tools/port_shaders.py` there reproduces `shaders.h` byte for byte). `profile` is
  macOS-only (fix 18).
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

- **64-bit, with no PS2 addresses.** No code reaches memory through a PS2 address: hardware
  registers are port functions (`port_timer_lines`, `port_display_field`), and the engine writes
  no VIF, GIF or DMA words: every Draw goes through a native override, `mgVif1Packet` is only a
  cursor, and the dead packet builders are stripped (`UNUSED_DEFINITIONS` in `tools/port.py`).
  Retail records keep their size:
  the DATA.HD4 index is read through `name_offset`, and the 4-byte pointers in photo records and
  `mgMEMORY_BLOCK` are `relative_pointer<T>`, an offset from the field to its target, which
  always lies in the same object or heap. On macOS the binary still re-executes itself once with
  ASLR off, so states load without moving pointers.
- **Upstream's `PORT` macro and headers**, with `-DPORT -DPAL`; `port.h` undoes the NTSC timing
  `PORT` alone selects.
- **The MWCC asm**: 54 functions are guarded out under `PORT`. 44 come from ChronicleTwo's four
  files; the port writes mg_frame's 7, `ClipBoxXZ` and `PlaneNormalXZ`, and `SendDMA` is stripped.
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
| `draw.cpp`, `mg_drawprim.cpp`, `mg_texture.cpp`, `mglib.cpp`, `prespr.cpp`, `gs.cpp`, `packets.c`, `graphics.c`, `dma.cpp`, `present.c` | the GS state machine, the texture cache, 2D drawing, frame and display | 1,630 |
| `mg_visual.cpp`, `visualmotion.cpp`, `mg_sprite.cpp`, `mg_shadow.cpp`, `water.cpp` | models, 3D sprites, shadows, water | 920 |
| `audio.c`, `midi.c`, `spu2.c`, `sound.cpp`, `ezbgm.cpp`, `iop.c` | the sound library and the `CSound` replacements | 1,560 |
| `input.c`, `memory_card.c`, `movie.cpp`, `movie_decode.c` | pads, cards, movies | 1,000 |
| `frame.cpp`, `collision.cpp`, `link_fixups.cpp` | asm replacements | 230 |
| `mods.c`, `port_mod.h`, `hooks.c`, `port_stats.h` | port mods: the hook tables mods register (`PORT_MOD`), their debug API commands and window panels, `port_setting` from the override folder's `dc2tk-port.cfg`; counters | |
| `port/debug/*` (the `dc2tk.debug-tools` port mod, debug build only) | debug window and API, scenario controls, frame capture, recordings, savestates | |

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
   compared with PCSX2. Water, depth of field and texture animation have been seen working, and
   lens flare works since the GS destination alpha test was added. It builds and plays on Linux
   x86-64 (checked in a container with software Vulkan; golden baselines differ by a few levels
   per channel, so Linux needs its own). Next: Windows, and Linux on real hardware.
8. **P3 Input.** Built. Gate: New Game, Palm Brinks, the first dungeon floor, played by a person.
   Next: play past the first floor's gate where it crashed, and melee hits.
9. **P4 Sound.** Built. Gate: effects, music and voices, heard against retail.
10. **P5 Saves and movies.** Built. Gate: a save made natively loads again (passed by replay); the
    opening movie plays (passed by replay); a save from a PS2 loads.
11. **P6 Engine mods natively.** Built. Gate: one mod with an engine change, a native called from
    a script, a table edit and a texture plays the same in PCSX2 and natively. Fixes 3, 6, 7.

## Later: resolution, widescreen, 144 fps

Wanted after the milestones, in this order: each is smaller than the next, and the last needs a
change the second-to-last does not.

### Resolution changing

Built: `--scale 1-4|auto` (auto renders at the smallest whole multiple that covers the window and
scales down to fit it exactly), `--msaa 1|2|4|8`, `--fullscreen` (Alt+Enter or F11 toggles it),
`--vsync`, a window size, and nearest or linear scaling; each is also a port setting
(`render.scale`, `render.window`, `render.fullscreen`, ...) and a function of `port_mod.h` 1.1,
which the display mod (`port/display/`, `dc2tk.display`) sets from its player-facing settings.
The window is HiDPI on macOS. Left to do:

1. **An in-game options screen** for these, instead of a mod set.
2. **The known 1x gaps:** render-to-texture cache textures stay 1x and shadow edges are not
   antialiased; the bitmap fonts and 2D art go soft when upscaled.
3. **Checks at 2x and above** of heat haze, depth of field and water.

### Widescreen

Built (`--aspect 16:9`, `render.aspect`, the display mod's `aspect`): the frame is widened to W
pixels (512 is 4:3) while the game's 512-wide layout stays as it was, centred in it:

- `mgSetPkFrameBuffer` makes every screen-sized (512-wide) frame buffer W wide, the back buffer
  and the work targets the engine draws whole-screen passes into (the dungeon draws each character
  into one that shares the frame's depth buffer, so they must stay the same size), with its
  XYOFFSET at 0x800 - W/2; the engine's own XYOFFSET writes there are moved by (W - 512) / 2, its
  full-width scissor widened and its other scissors moved, so 2D draws in the middle
  (`port/src/draw.cpp`, which keeps each target's shift).
- The 3D view widens (Hor+): `mgSetRenderInfo` passes W for the screen box and guard band and
  keeps its 512-based projection and aspect; frame culling uses W (`port/src/frame.cpp`).
- Reads of the back buffer (copies, stores, 2D sampling) see its 512-wide middle, so the effects
  that read the frame back work as before, in the middle.
- Full-frame draws are widened: clears, fades (`DivSpriteScreen`), the frame border, and the
  character shadow pass (`shadow_work`).
- Only the town (walking, Georama, waiting) and dungeon floors (exploring, events) are wide
  (`port_world_shown`); menus, the title loop (its costume, load and options screens draw models
  that a wide frame loses) and 2D-only screens are shown 4:3.

Left to do:

1. **Effects in the side areas:** depth of field, heat haze, water refraction, outlines, the
   photo camera and cross-fades act on the middle only.
2. **The HUD** stays in the middle; anchoring the edge pieces (health, minimap) to the sides.
3. **Content limits:** cutscenes framed for 4:3 may show scenery or actors off camera; movies
   stay 4:3.

### 144 fps

Built as interpolation (`--smooth HZ`, `render.smooth`, the smooth mod `port/smooth/`,
`dc2tk.smooth`). The game logic stays at its 25 or 50 Hz (`mgFrameRate`; only five engine sites
scale by `mgGetNowFrameRate`, so a variable time step would mean rewriting every system and every
script that counts frames):

- `render.c` records each frame: every pass (its target, load operations and commands, with the
  model uniforms, bone sets and a copy of the 2D vertex data) and every texture copy, in order.
  Textures released while smoothing are freed two frames later, so a recording never points at a
  freed one.
- At the game's present the recording replaces the frame shown: `render_replay(alpha)` re-encodes
  it with each model's uniforms and bones blended between the frame before (alpha 0) and the last
  (alpha 1). Models are matched by resident mesh, each to the copy of the frame before nearest in
  its own world and screen matrices and its bones (the camera's matrix is the same for every
  model, so it cannot tell copies apart); one that moved by more than a quarter of them snaps.
- `smooth.c` paces the replays at the display's refresh (or the chosen rate) while the field
  timer waits (`port_graphics_wait`), never starting one that would run past the next field; the
  picture is one update behind.
- A replay costs 0.6 to 0.8 ms of CPU at 1x on the M-series Mac; the debug build's `stats` shows
  `replays` and `replay_us` per frame, and `between alpha=X` returns an in-between frame.

Left to do:

1. **What the CPU places on the screen** (particles, 3D sprites, water, shadows drawn as 2D)
   stays at the game's rate; recording them in world space would let them blend too.
2. **Present pacing with vsync:** each replay blocks on the display, so fewer fit between the
   20 ms fields; a mailbox present (where the platform has one) would not block.
3. **Mid-frame uploads:** a texture uploaded after an earlier pass used it is replayed with its
   last contents.
4. **Blending effects** that read the frame (the castle's lamp glow) come out slightly dimmer in
   replays.
