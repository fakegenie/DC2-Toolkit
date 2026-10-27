# Plan

The engine in dc2tk, mods that change it, and a minimal native port for macOS, Linux and
Windows. The facts about
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
| P2 graphics | the engine draws through SDL_GPU (replaced the emulated DMA, VIF, VU1 and GS) | `tools/port.py --movies` builds clean (149 units, 40 files, no stubs); `port/tests/run.py` passes on macOS; 50 Hz in a window | PCSX2 comparison (water, lens flare, depth of field and texture animation have since been seen on screen) |
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
4. **Built, not yet run on a real engine build: table units keep their profile.** `compile_mods`
   no longer strips Satan's Fiddle's profile from units whose tables are written as source, and
   after the link `engine_tables::check_built` compares every field of each edited table at its
   symbol's address (read from the `.xMAP` by `map_symbols`) with what the edit wrote, refusing
   the build on any difference. `the_built_engine_must_hold_every_edited_field` and
   `the_link_map_gives_each_symbol_its_first_address` pin it. Still to do on a machine where
   Kazarov builds: a mod with a `data:monsters` edit and an engine mod, to confirm the `.xMAP`
   lines are `ADDRESS NAME...` as `map_symbols` reads them and that `check_built` passes.
5. **Done: `data:parts` from source.** `PartsInfoData` and `MiniMapInfoData` are tables in
   `engine_tables.rs`; a part's name is a `Named` field, whose bytes are where the record's pointer
   points and whose source is the string literal, so a name edit rewrites the literal and a changed
   pointer is refused. `part_names_change_their_strings_and_never_their_pointers` pins it.
6. **Done: `--native` without FILE** links where `tools/port.py` puts the binary (`build/port/dc2`,
   `build/port/windows/dc2.exe`), wherever dc2tk runs from; `--native FILE --dry-run` is refused,
   and `--dry-run` alone links `build/port/checked-native` to check it.
7. **Done: one native build.** `tools/port.py` is the only one. `dc2tk build --native` and
   `mod check --native` run it with `--engine-mods` (the copy `prepare_mods` made; its mods' files
   go through `native_prepare.py`), `--engine-sources` (`build/port/tables`, the table units with
   the edited tables written in), `--mod` for each mod with port code, `--executable-sha256` (for
   `executable.c`) and `--output`. A binary linked outside `build/port` gets an rpath to
   `build/port/lib` (Windows: the DLLs are copied beside it). `port.py` removes files from
   `build/port/copy` that no longer come from the engine or a mod.
8. **Done:** the SDK scripts build with as many jobs as the process's CPU affinity allows.
9. **Done: debugging in the shipping binary.** `DC2TK_SCREENSHOT` and `DC2TK_PRESS` are gone (the
   debug API's `screenshot` and `press` replace them); the sound dump is the debug build's
   (`debug_audio`, `DC2TK_SOUND_DUMP`); `cdvd.c` logs opened files and archive members, and loads
   the archive index for that, only after `port_cdvd_log_files(1)`, which the debug build calls.
10. **Done:** `dma_chain.c` is deleted with the rest of the emulation.
11. **Done:** `port/tests/run.py` writes its patched sources and objects under `build/port/tests/`,
    and `-DPORT_SOUND` and its conditionals are gone. Every unit keeps `-I engine/port/src`:
    `mg_drawprim.cpp` and others include `vu0_lanes.hpp` from there.
12. **Done: the effect-heap substitutions.** `EFF_SPT_RUNTIME_QUADWORDS` is the script, the
    character and the stack, as retail counts them, plus the call frames beyond the quadword retail
    leaves to the fit's slack (`2 * sizeof(RS_CALLDATA) / 16 - 1`): 161 with PS2 sizes, retail's
    `0x7D + 0x24`. `AssignCharacter`'s `+ 3` and the call data's round-up (no change on 64-bit)
    are dropped; `port/tests/effect_heap.cpp` passes.
13. **Done: build copies.** `tools/port.py` holds `build/port/build.lock` while it builds;
    `prepare_mods` holds `build/engine/mods-<hash>.lock` while its copy is in use and removes all
    but the four copies used last; a mod's engine files without a fetched engine say "run
    `dc2tk engine fetch`".
14. **Done:** `definitions/natives/` regenerated: `_SHOT` has one accessor, and `_CHECK_PAUSE` and
    `_GET_BTN` take int then out, as the engine reads them.
15. **Done:** the style items, and `python_in_container` and `export_override` run `python3 -I`.
16. **Done:** `build/validation` is gone.
17. **Savestates on Linux.** Done with the redesign (savestate entry 2 below): states keep the
    `game_data`/`game_bss` sections, and a non-PIE binary never needs relocation.
18. **`profile` off macOS.** `port/debug/core.cpp` runs macOS's `/usr/bin/sample`; on Linux and
    Windows it only says to use `perf` or the Windows Performance Recorder.
19. **Done: golden baselines per platform**, in `build/port/golden/baseline-macos`, `-linux` and
    `-windows` (Lavapipe's pictures differ from Metal's by 1 to 3 per channel).
20. **Done:** saves default to `SDL_GetPrefPath("DC2 Toolkit", "DC2")/saves` on every platform.
21. **Done: the costume screen** draws Max without white patches on macOS (release and debug, 4:3
    and 16:9, checked 2026-10-26); Linux not rechecked.
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
25. **Done: diagonal running on a gamepad.** A round-gated pad gives about 0.71 per axis at a full
    diagonal, which the engine's per-axis dead zone turned into 0.74, below the town's run at 0.8.
    `port_input_square` (`port/src/input.c`) stretches SDL's sticks from circle to square, as the
    touch stick already did; `port/tests/input.c` checks every direction runs at 90% push and the
    dead zone stays. Not tried on a real phone controller.
26. **Done: Sindain's sides at 16:9.** `CEditMap::DrawRiverMask` drew its whole-screen mask only
    `mgScreenWidth` wide, so the river's sky reflection showed in the side areas. `port.json`
    widens it like the clears.
27. **Done: Max's eyes closed at rest in the item menu** with `render.smooth` on. Between frames
    replay GPU copies but not uploads, so replaying the frame that filled a texture's untouched
    copy filled it mid-blink. `gs_texture_for` (`port/src/draw.cpp`) now uploads the decoded
    picture into the untouched copy and copies it to the texture. Seen only with smoothing on.

### Next: debugging and states that last

What the fishing work kept needing, in the order to build it:

1. **Engine variables by name.** Done: `peek name=CastDist [type=f32|i32|u8|ptr|str|hex] [count=N]`,
   `poke`, `symbols match=` and `watch` (a Watch tab in the debug window) read and write any global
   or file static through the symbol table `tools/port.py` writes beside the debug build
   (`dc2-debug.symbols`), on every platform. `status` also reports the player's and the camera's
   position.
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
2. **No mid-frame splits.** Done. Each pending command keeps its target, and draws to another
   target that nothing pending reads (and that neither reads the frame nor clashes with its depth)
   are queued beside the frame's and encoded before them, so texture animation no longer ends the
   main pass. A side draw clashes with the frame's depth only when one of them writes it: shadows
   that only test the frame's depth queue beside it. A draw to a side target that pending draws
   still read copies its resolved picture first and points those readers at the copy
   (`detach_readers`), so the depth of field strips keep reading the halved frame while the shadows
   are drawn into the same slot. A flush for a copy, or for a draw that reads the frame, encodes
   only what has to come first (`flush_for`): a side target that nothing pending reads, that reads
   nothing pending and does no depth work waits, so the swamp's texture animation into slot 22
   shares a pass with the depth of field's halving. A pass that neither tests nor writes depth gets
   no depth buffer and no depth mirror. Unpaced with 4x MSAA (`stats`): swamp 10 to 8.2 passes a
   frame, Palm Brinks 9.2 to 8; attachment loads and stores, counted from the passes without
   compression, 231 to 152 MB a frame in the swamp and 192 to 135 in Palm Brinks at scale 1. Every
   frame of both recordings is identical (hashed at each present; the loading screen's frames
   63-85 vary between runs of the same build). On the M1 Pro at scale 1 the game is CPU-bound and
   the change is lost in the noise (about 650-800 ticks a second either way); at scale 3 it is 1-4%
   faster when the GPU stays in its fast state (swamp 186 to 190 ticks a second, Palm Brinks 197
   to 203), and the GPU's clock state swings runs by 30%, more than the change. What still splits
   the frame is real: the depth of field reads the scene before the characters, its strips need the
   halved copy resolved, the shadows and the fire animation each need their own version of slot
   22, and the heat haze copies the frame into `fire_work` every frame before the HUD is drawn, even
   when no fire reads it. Copy-on-write for MSAA targets was tried before and dropped: the redraw
   that refills a fresh MSAA texture from its resolved copy is lossy at edges. Left: the texture
   animation and fire draws into slots 17 and 22 are 128x128 but pay for a whole 640x512 4x MSAA
   target, as `render_target` makes every target at least that size with the frame's samples; and
   nothing here was measured on a tile GPU.
3. **The game on its own thread.** Done. SDL only lets the window's thread acquire a swapchain
   texture, so the game moved instead: `main` starts it on an SDL thread (`run_game`) and the main
   thread runs `port_present_run`, which pumps events, hands them to the game through a queue
   (`port_present_frame` drains it), and presents. The game still draws every frame and every
   between frame itself, then copies the finished picture into a pool of three per window
   (`publish`); the main thread shows the newest one it has not shown (`render_show_screens`) and
   may block on `nextDrawable` without touching the game's timing. The debug window publishes its
   ImGui canvas the same way. Window calls from the game (show, hide, fullscreen, size, vsync, text
   input) go through `SDL_RunOnMainThread`; quitting and iOS backgrounding park the game at its
   next field (`park_game`), and an exit from the game thread stops the main loop first
   (`stop_main`). Before it, with smoothing the tick shows a kept copy of the frame before instead
   of replaying the whole frame at alpha 0 (GPU 6 to 3.7 ms a paced tick), and playback draws
   between frames too (`scripted_tick` waits with `smooth_until`). Swamp, front window, 98 Hz,
   `--smooth 0`: 25.0 ticks a second with the slowest frame 41 ms (it was 70-79 ms when a between
   frame blocked in `nextDrawable`), 105 shown a second; unpaced 445 ticks a second. `shown` in the
   debug API returns the picture last handed to the window.
4. **Less per tick.** Done. Profiled with `sample` on the game thread during unpaced playback:
   texture animation re-sent its textures every frame (`mgCDrawManager::ReloadTexture`), so each
   animated texture was decoded and uploaded again and the cache was scanned for it; a texture the
   GPU has drawn into now keeps an untouched copy on the GPU (`CachedTexture.pristine`) that is
   copied back when the same pixels are re-sent, and the cache is found by image and render slot
   (`by_image`, `by_render`) instead of scanning 4096 slots. Consecutive skinned draws with the same
   bones share one bone set, and `draw_group` hands `render_mesh` whole runs of resident faces (swamp:
   4052 mesh calls a frame to 831). Because the restore is a recorded copy, between
   frames now replay texture animation from the same starting picture as the real frame instead of
   animating it twice. Unpaced with 4x MSAA: swamp 445 to 700 ticks a second (game 2.16 to 1.35 ms,
   GPU 1.9 to 1.35 ms), Palm Brinks 395 to 576 (game 2.4 to 1.65 ms), real frames identical. What
   is left is the engine's own logic and encoding ~840 commands a frame. `texture_checksum` stays:
   at about 5% of a tick it is cheaper than trapping writes to game memory.
5. **60 ticks.** Not done, and not needed: smoothing already shows a frame every refresh. Menus run
   at `mgFrameRate` 1 and movement scales by it, but it is read in 75 places in 20 files while
   hundreds of timers, animations and events step once a tick, so a faster tick would mean auditing
   every one of them.

### Android

`tools/android_sdk.py` builds shared SDL3 and static FFmpeg for arm64-v8a with the NDK (27.1.12297006 locally and on
CI); `tools/port.py --android` compiles the engine with clang 23 against the NDK sysroot (`-fPIC`) and links
`libmain.so` with the NDK's clang++; `tools/port_package.py --android` makes the APK without Gradle and signs it with
`port/android/dc2.keystore` (CN=Fake Genie, password `dc2`, public on purpose). The `android`
release job uploads it. Checked on the Marathon7_API36 emulator (arm64, API 36): launcher, disc and mod picking
through `content://` URIs, the game with movies under SDL_GPU on Vulkan, touch controls, a save state loaded in a later
launch at another `libmain.so` address, and leaving and returning to the app. Not yet tried on a phone. On the
emulator its keyboard has a D-pad, so SDL counts a gamepad and Smart hides the touch controls; SDL's OpenGL ES renderer
drew no launcher backdrop there, so the launcher asks for SDL's Vulkan renderer (`SDL_RENDER_DRIVER` in the manifest),
which resets its textures when the app comes back from another activity (the launcher remakes them on
`SDL_EVENT_RENDER_DEVICE_RESET`). Save states do not survive an app update: unlike iOS, nothing keeps the old build.

### Windows

Built and checked on a Windows 11 x64 box (2026-10-26). `tools/port_sdk.py` unpacks llvm-mingw 20261020 (LLVM 23.1.4,
ucrt, sha256-pinned) into `build/port/llvm-mingw` and builds SDL3 and FFmpeg with it into `build/port/windows-sdk`;
`tools/port.py --movies` links `build/port/windows/dc2.exe` with 0 stubs (148 units, 57 other files);
`port/tests/run.py --iso pal.iso` passes there, the retail logo and tutorial movies included; the exe, started on
`pal.iso` in a real window, draws the language select under Vulkan, and the launcher opens; `tools/port_package.py`
packs `dc2-<version>-windows-x64.zip`. The shared changes were checked on Linux too (WSL Ubuntu, llvm-mingw's
own Linux clang 23 with the system's GNU ld): the port links with 0 stubs and `port/tests/run.py` passes. Not yet: a
human playing past the language select (input needs the window focused), sound on a real device, a savestate taken and
loaded on Windows, a save through the game's menu, and the release job's first run (`windows-latest`).

How it is built:

- **Toolchain**: llvm-mingw, not MSVC: Itanium mangling, which the ALIASES, OPERATORS and stubs depend on. Linked
  `-static` (libc++, libunwind, winpthreads) and `-mwindows`; the only DLLs beside the exe are SDL3, avcodec and
  avutil. FFmpeg's configure runs in Git for Windows' bash and builds with llvm-mingw's `mingw32-make`, inside its own
  unpacked tree: with absolute source paths its header install is one command line longer than Windows takes, and it
  fails without a word.
- **Linking**: lld's MinGW driver has no `--defsym`; the ALIASES are lld-link's `/alternatename` through `-Xlink`.
  llvm-objcopy cannot localise, weaken or rename sections of COFF objects: LOCALISED symbols are renamed to names
  nothing else uses, the upstream objects are linked last with `--allow-multiple-definition` (the first definition
  wins, as the strong one does over a weak one on ELF), and `.data`/`.bss` (with their `$` groups) become
  `gdata`/`gbss` by writing the names in place, as long as the old (PE section names are at most 8 characters). The
  same option covers an inline function's COMDAT copy beside its out-of-line definition (`ClsMes::Init`,
  `CMapParts::CMapParts`), which ELF lets the strong one win and COFF calls a duplicate.
- **Windows names**: the PS2 kernel's `CreateThread`, `GetThreadId` and `TerminateThread` are kernel32 functions too.
  Every name in our objects that the default import libraries define becomes `ps2_NAME`, so the port's definition (or
  a stub) is what the engine gets and the static runtime never calls the port's.
- **LLP64**: `long` is 32 bits. `port.json` makes the costume bits, `mgCDrawPrim::Direct`'s register data, the GS
  scissor shifts in `nd_meswin.cpp`, `0x80UL << 32` in `inventmn.cpp` and the password's 8-byte groups `u64`/`s64` on
  every platform (a sweep of every plain `long` in the patched engine found no others that need 64 bits). The rest are
  harmless at 32 bits: `dbg_font.cpp`, `snd_mngr.cpp` 1912, `nameregi.cpp` 698, and the movie pts/dts of `movie.hpp`
  and `libmpeg.h` (the engine's `movie.cpp` is not built). `-mno-ms-bitfields` keeps the engine's bitfield layouts
  (no SDL or FFmpeg header has a bitfield), and `-D_FILE_OFFSET_BITS=64` the 4.4 GB disc's offsets.
- **Fibers** (`port/src/fiber.c`): a Win64 switch saving rbx, rbp, rdi, rsi, r12-r15, xmm6-xmm15, rsp, the resume
  address and the thread information block's StackBase and StackLimit, on the caller's stacks as before.
- **Files**: `port/windows/posix.c` (`port/include/windows_posix.h`) gives `memory_card.c` directory descriptors that
  keep a path, and the `*at` calls on them, plus `pread`, `fstatvfs` and `fsync`; files open in binary mode. The disc
  opens with `open`, as SDL's Windows streams have no descriptor. The manifest makes the C runtime's code page UTF-8,
  so `char` paths are SDL's UTF-8 everywhere. Windows has no symlinks to refuse, so the card test leaves those checks
  out there.
- **Savestates** read the PE section headers through `__ImageBase` for `gdata` and `gbss`; the image keeps its fixed
  base (`--disable-dynamicbase`), as the non-PIE Linux build does.
- **Console**: `port/windows/console.c` attaches to the console it is started from, unless its output already goes
  somewhere.
- **Tests**: `port/tests/run.py` adds the shim to every link and lld-link's `/force:unresolved`, as COFF's lld reports
  undefined symbols that only discarded sections name, which ELF's lets go; the collision objects are not weakened but
  come after the engine's.

The debug build is one implementation on every platform (2026-10-26): `port.py --movies --debug` links
`build/port/windows/dc2-debug.exe`, `tools/golden_port.py` passes there against Windows baselines (written with
`--update`, twice in a row identical), and on Linux (WSL, clang 23) it links and its API, watches and restart work too.
What is still per platform is small:

- **Sockets** (`server.c`): Winsock and BSD differ in a few helpers (closing, non-blocking, not inherited, SIGPIPE).
- **Crashes** (`log.c`): signals with `sigaltstack` on macOS and Linux, an unhandled-exception filter on Windows. The
  stack is walked by libunwind's `_Unwind_Backtrace` there, through the signal frame, and by Windows' own
  `RtlVirtualUnwind` from the exception's registers here, as libunwind stops at a leaf function without unwind
  info. The report (addresses, then each named from the symbol table, then the crash recording) is shared.
- **Log capture**: `_pipe` on Windows, and there the standard handles are pointed at the pipe as well, since `dup2`
  moves them only in a console program and child processes get the handles.
- **`profile`** stays macOS's `sample` (started through `SDL_Process`); elsewhere it says what to use instead.

The rest is SDL3 or the port's own data: threads and the log lock, folders (`SDL_CreateDirectory`), the time,
the environment (`SDL_setenv_unsafe`, and `memory_card.c` reads `DC2_SAVE_ROOT` with `SDL_getenv`, which a restart
hands on), and the restart for a recording from launch: a new process with the same arguments (`SDL_CreateProcess`,
standard I/O inherited), after this one lets go of the debug port; it hides its window, stops its sound, waits and
exits with the new one's status, so a tool waiting on the process it started still waits on the right one. Watches
read only inside the program's sections or the engine's arena and write only to writable ones, so `poke` into code
is refused rather than a crash. Symbols come from `BINARY.symbols`, which `tools/port.py` writes beside the debug
binary from the binary's section headers (Mach-O, ELF or PE, read in Python), `nm` of the binary, and `nm` of each
linked object for which object a symbol is from (a static name in several objects goes to them in link order);
symbols without a size (Mach-O, COFF) run to the next symbol or the section's end, and the loader moves every address
by how far `port_build_id` has moved since the link. The macOS path of this is not yet checked on a Mac.

Open on Windows:

- `dc2tk build --native` runs `tools/port.py` (item 7), so it builds on Windows too; not yet run there.
- `memory.c`'s ASLR respawn is macOS only and not needed with the fixed base.

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
  Put `/opt/homebrew/opt/llvm/bin:/opt/homebrew/bin` first on PATH: `/usr/bin/python3` is 3.9,
  too old for the tools, and Apple clang's libc++ clashes with the engine's `abs` and `random`.
- **Kazarov's container on this Mac:** needs the colima x86-64 VM Kazarov asks for, which is not
  installed. OrbStack (`BUILDER=docker DOCKER_CONTEXT=orbstack`) builds the image but the
  compile stops: `wibo` gets SIGSTOP before loading MWCC, as Rosetta runs no 32-bit x86 code.
- **Building the port:** `python3 -I tools/port.py --movies`, then `build/port/dc2
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
  macOS writes a report for every crash to `~/Library/Logs/DiagnosticReports/dc2-*.ips`
  (JSON after the first line), with the faulting thread's frames.
- **The debug build:** `python3 -I tools/imgui_sdk.py` once (Dear ImGui 1.92.9b into
  `build/port/imgui-sdk`), then `python3 -I tools/port.py --movies --debug` also links
  `build/port/dc2-debug` (`build/port/windows/dc2-debug.exe` on Windows) and its symbol table
  `dc2-debug.symbols`; a crash prints its stack, named from that table, and saves a recording up
  to it. It opens a second window (F1 shows or hides it, F5 pauses, F6
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
  docker build -t dc2tk-rust -`); `python3 -I tools/port_sdk.py` builds SDL3 3.4.18 and FFmpeg
  8.0.1's MPEG-2 decoder from source into `build/port/sdk`.
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
  on macOS and Linux with `dc2tk build --native`, which drives clang itself, without CMake, and on
  Windows with `tools/port.py` only.

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
4. **E4 Tables and natives.** Built; the table byte check (fix 4) still needs one real engine build.
   Gate, the user's: a shop edit and a mod's native work in PCSX2.
5. **P0 Compile spike.** Done.
6. **P1 Headless boot.** Done: the ISO boots to the language selector.
7. **P2 Graphics.** Built natively, 50 frames a second in a window on macOS. Gate, the user's:
   compared with PCSX2. Water, depth of field and texture animation have been seen working, and
   lens flare works since the GS destination alpha test was added. It builds and plays on Linux
   x86-64 (checked in a container with software Vulkan; golden baselines differ by a few levels
   per channel, so Linux needs its own), and it builds and draws on Windows x64 (see "Windows").
   Next: Linux on real hardware.
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
2. **The known 1x gaps:** done, checked at 1x, 2x and 4x on the language screen, title, attract
   demo, Palm Brinks (square, canal, main menu), the castle dungeon's floor select and floor 1.
   Scale 1 is unchanged (golden passes; every shot but one matches; see the last point).
   - Render-to-texture cache textures: a decoded texture the engine draws into is replaced by a
     scaled copy (`scaled_cache_texture` in `draw.cpp`) the first time a model, 3D sprite, water,
     untextured 2D draw or draw from a scaled texture lands in it, or a scaled target is copied
     into it. Draws of 1x art into it (texture animations) leave it 1x, so those stay as sharp as
     the art. UVs are normalised, so PS2 texel addressing is unchanged. In the scenes tried only
     Palm Brinks' canal water (`water`, ripples drawn as ~1000 untextured triangles) is upgraded;
     the menu backdrop `menuwork2` comes from a CPU read-back (`mgStoreImage`) and the other
     effect targets were already scaled render targets. The canal looks almost the same
     (MAE 0.1 over a crop at 2x and 4x).
   - Shadow edges: the shadow composite samples the volume mask as CT24 with AEM, so each texel is
     either shadow or not, which left a staircase at the scaled resolution. Such draws (TEXA, AEM,
     linear, scaled source) now use a variant of `draw.frag` built with `SPREAD`
     (`tools/port_shaders.py` builds `draw_spread_frag`, `PipelineKey.spread` picks it). It expands
     alpha per texel before filtering, as the GS does, and averages four taps a quarter texel
     apart, so edges fade over about one pixel. It returns early where the 3x3 neighbourhood is
     black. Putting this in the one shader instead cost every draw ~0.4-1.2 ms at 4x.
   - Sharp 2D: a linear sprite from a 1x texture into the displayed frame whose texels map 1:1 to
     layout pixels (within 1/32) is drawn nearest, with its UVs shifted so pixel centres land on
     texel centres (`one_to_one`, `snap_texels`). Scaled or rotated art, and 2D drawn into work
     targets that are sampled later, stay linear. The menus' text and frames were already
     nearest or scaled; the visible case is the floor-select marker, which now matches 1x.
   - GPU cost (`tools/bench.py --unpaced`, gpu_ms mean, two runs each): at 2x Palm Brinks 1.41 ->
     1.46 ms, swamp 1.20 -> 1.19; at 4x 3.41 -> 3.66 and 3.15 -> 3.37.
   - Left: no scene with 3D drawn into a decoded texture (portraits, photo) was found, so that
     path is only exercised by the canal water. Not checked with `--msaa` or 16:9. Adding any
     field to render.c's `Texture` or `PipelineKey` changes 13-21 pixels by 1 in the town menu's
     frame at 1x (with all new logic disabled too, and with nothing else changed), so some
     pre-existing code depends on memory layout; not found.
3. **Checks at 2x and above** of heat haze, depth of field and water: done, at 2x and 4x in
   Balance Valley (torch haze, distance blur), the fish race course (map 96, a CWater surface) and
   the Rainbow Butterfly Wood. Haze and water matched 1x (on/off difference 211/208/220 for haze,
   721/716/715 for water at 1x/2x/4x). Depth of field blurred much less when scaled, as it blurs by
   halving the frame and the halved frame kept the full resolution. `DepthOfField` now turns on
   `port_ps2_sampling`, and its draws from scaled targets sample the texture as the GS would at 1x
   (bilinear between 1x texel centres, `PS2_SAMPLING` in `draw.frag`), so the blur matches 1x and
   stays smooth: in the blurred cliff at 4x the difference from 1x fell from 640 to 182 (MAE over
   a 120x80 crop). Left: the lens flare halves and thirds the frame the same way and was not
   looked at.

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

1. **Effects in the side areas:** done. Depth of field, heat haze and water refraction cover the
   whole width:
   - depth of field already blurred the sides; its halved frame is now W/2 wide (it squeezed W
     into 256) and starts at -side, so the blur is as wide as at 4:3 and the second level fits;
   - heat haze copies the whole frame into `fire_work` (entered `port_wide_width()` wide) and
     clamps its wisps to the wide screen instead of the middle;
   - water copies the whole frame to x 0 of `water_work` whatever shift that block's base last
     had (texture animations bind it at 0, the shadow pass at side), and `port/src/water.cpp`
     places the surface by the frame's XYOFFSET and reads the copy at layout x + side. Before, the
     surface was drawn 85 px left of the scene and its refraction showed objects 85 px away (a
     ghost of the rock at the race course);
   - outlines already covered the sides (`COutLineDraw` and `GetDrawRect` use the wide width): a
     woman in the left side area of Polly's Bakery has the same edge as the baker in the middle;
   - the photo camera shows 4:3 while it is up (`port_town_shows_world` and
     `port_dungeon_shows_world` are false under `NowTakePhoto()`), as the photo is a 64x64
     thumbnail of the middle kept in the save, so the viewfinder shows what is taken;
   - cross-fades store the whole wide frame: `cross_f` and `capture` are entered
     `port_wide_width()` wide, their pixels go to buffers of the port's own
     (`port_cross_image`), as the engine's spare buffers hold only 512 x 480, and
     `port_store_screen` reads the frame around the middle; `DivSpriteScreen` and the wipes read
     the wide image, where they smeared the edge pixels over the sides;
   - the event sepia stores and draws the whole frame the same way (`port_sepia_image`);
   - event screen sprites: an untextured full-screen sprite (the black of a title card) reaches
     the edges, and a textured one that covers the 512 x 416 still area (the drawings of event
     207, the Griffon picture) gets black bars beside it that fade with it
     (`port_screen_bars`).
   Left: the event movie's black backing and the mono flash have no case to test and stay in
   the middle.
2. **The HUD:** done. `port_hud` spreads `DrawStatusBord` and `WarningGage2` (health, weapons),
   the small minimap (its scissor moves with it; the large map stays centred), the boss gauge at
   the bottom left (`boss_life_gage`) and the town's Georama icon and balance bars
   (`DrawEditSystem`). Left in the middle by design: message windows, level-up and damage
   numbers, and the floor title.
3. **Content limits:** checked at 16:9 in events 11030 (the pump room) and 11100 (the
   floor 7 boss intro) in the Underground Water Channel, 12080 (the Dwarves' house) in
   Sindain, 11091 (the mayor's office) and 207 (the station): no actor pops in or out at the
   sides and the scenery reaches the edges, so event cameras stay wide. A fixed camera in a
   small interior (Polly's Bakery) shows black past the room's walls. Movies stay 4:3.

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

1. **What the CPU places on the screen** (particles, 3D sprites, water, shadows drawn as 2D).
   Done for 3D sprites and water; shadows already blended (`blend_shadows`):
   - Every `mgC3DSprite` item (the effect manager's particles, dust, fire) is recorded with its
     world position, its object as key and the camera it was projected with (`render_sprite`).
     `match_sprites` pairs each with the nearest of the same object and texture in the frame
     before (within 40 units); the replay projects the blended position with the blended camera
     and blends the corner offsets, colour and fog. One with no match follows the camera.
   - Water (`render_water`) follows the camera shift, and its refraction coordinates slide with
     it, instead of being paired by vertex count (which changes as strips are culled).
   - Measured with `between alpha=0` against the real frame before (pixels off by more than 24;
     0 would be perfect), before and after: Finny Frenzy race 763 to 299, swamp 228 to 70,
     fire 470 to 418, pool 657 to 97-125 (three runs), palm 56 to 56. Real frames and alpha 1 are
     unchanged.
   - Cost: `stats` `replay_us` per replay in the debug build (fresh launch per scene, two rounds,
     nothing else running), before and after: pool 1.44-1.53 to 1.81-1.93 ms, swamp 1.89-1.97 to
     1.94-2.05 ms, fire 0.83-1.19 to 1.19-1.38 ms, so 0.1 to 0.4 ms more, mostly placing sprites
     and water on the CPU.
   - Left: GS draws the engine projects itself (`mgTransWorldPrim`, `mgTransWorldPrim3DSprite`:
     combat hit and death effects, sword trails, town rain and ripples, gift marks) are still
     paired by signature only and stay at 25 Hz when their count changes. Following them is
     possible (a projection call since the last PRIM write plus a GEQUAL/GREATER depth test marks
     one) but none showed up in the test scenes, so it was left out. Texture animation (waterfalls,
     scrolling streams, flame frames) steps at 25 Hz.
2. **Present pacing with vsync.** Done. Since the game moved to its own thread a replay no longer
   blocks on the display, but `smooth_until` still kept a whole refresh free before the field with
   `--vsync` (half a refresh without), from when it did. It now keeps a quarter of a refresh either
   way. Swamp and Palm Brinks recordings, `--smooth 0`, background window, 98 Hz display, shown frames
   counted on the main thread: with `--vsync` 75 to 93-94 shown a second and the slowest shown gap
   in a typical second 23.5 to 15 ms; without it 90-91 to 93-94. The tick's slowest frame stays at
   42 ms and every published frame is shown. Holding a replay until the main thread has taken the one before
   (`shown == serial`) made no difference on an idle machine, so it was left out; with five other
   games running it had cut overwritten frames from 64 to 0.
3. **Mid-frame uploads.** Not needed. A probe on every `render_upload`, counting uploads to a texture
   that a pass or copy recorded earlier in the same frame had used, found none in the attract demo,
   both recordings, six menus over 3D (main, shop, inventory, georama, character, options), the
   first floor of four dungeons and the castle prologue event (texture animation restores through a
   recorded copy). Replays that run while the next frame is partly drawn do not see its uploads
   either: those wait in that frame's command buffer, which is submitted after the replay's.
4. **Blending effects that read the frame.** Done. No dimming is left: in Palm Brinks at night the
   lamp regions match the real frame at `between alpha=0.5` with a fixed camera (61 of 21600 pixels
   off by 1), and over 80
   presented pictures each replay has the lamp brightness of the frame it ends on. What did differ
   at `between alpha=1` were sparkles and particles a pixel off (castle prologue, 25 of 30
   samples): `was + (now - was) * alpha` and the camera shift through an inverted view are not
   exact at 1. At alpha 1 the replay now draws the recorded vertices, models and bones as they were,
   and an unchanged camera adds no shift; the castle (56 samples) and Palm Brinks at night (scale 2,
   4x MSAA) now match pixel for pixel.
