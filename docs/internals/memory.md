# Memory budgets

DC2 loads maps, character models, textures, event scripts, and sound banks into fixed-size memory buffers and stacks. This page documents each runtime memory budget, where the engine configures it, the largest asset allocation on the PAL disc, and the remaining headroom.
- For how `dc2tk build` checks modified files against these budgets, see [staying within memory](../guide/memory.md).
- For record-count limits in gameplay data tables, see [game data](data.md).

All numbers come from static analysis of the ChronicleTwo decompilation (`ps2/src/` and assembly for unmatched functions) and exact measurements across the extracted PAL disc (`SCES_511.90`).
- **Certain**: The buffer size is a hardcoded engine constant and asset usage is computed exactly.
- **Estimated**: Either the buffer bound or the per-asset allocation formula is approximated.
- **Quadword accounting (`mgCMemory`)**: The allocator measures capacities in 16-byte quadwords and rejects any allocation that reaches the final quadword (`used + n >= size`), so a stack of `n` quadwords holds at most `(n - 1) * 16` bytes.

## Summary

| Memory Budget | Capacity (Bytes / Units) | Largest Use on the PAL Disc | Remaining Headroom | Status |
|---|---|---|---|---|
| Global file read buffer (single file loaded whole) | `3,200,000` B | `e612_griffon.chr` (`3,197,312` B) | `2,688` B | Certain |
| Town map files in read buffer (with loading screen packets) | `3,120,000` B | `s48` (`3,021,376` B) | `98,624` B | Certain |
| Dungeon map files in read buffer (with loading screen packets) | `3,040,000` B | `d05f02` (`2,744,384` B) | `295,616` B | Certain |
| Map event script buffer (`.stb`) | `312,816` B | `map/d/d05/d05_5.stb` (`309,664` B) | `3,152` B | Certain |
| Map part placements (town map / interior) | `320` placements | `m08` (`230`) / `m05` (`245`) | `90` / `75` | Certain |
| Map part placements (dungeon floor) | `400` placements | `d06b02` (`102`) | `298` | Certain |
| Texture blocks for a dungeon map | `15` blocks | `d06f02`, `d06f03` (`14` blocks) | `1` block | Certain |
| Texture blocks for an interior | `40` blocks | `i04h01a` (`16` blocks) | `24` blocks | Certain |
| Texture blocks for a town map (alone / with interior) | `64` / `24` blocks | `s53` (`32` blocks, loaded alone) | `32` blocks | Certain |
| GS VRAM per texture block | `4,768` GS blocks + Z-buf cache | `s01` (fullest block leaves `12` GS blocks) | `12` GS blocks | Certain |
| Max base model pack (dungeon party stack 0) | `1,096,000` B | `c01_base3` (~`1,080,672` B) | ~`15,000` B | Estimated use |
| Melee weapon, ranged weapon, hat (party stacks 2, 3, 4) | `112,000`; `74,400`; `48,000` B | `ws` (`104,496`); `wg` (`71,968`); `ha` (`47,792`) | `7,504`; `2,432`; `208` B | Estimated use |
| Single VU1-skinned mesh | `3,198` vertices, `32` bones | — | — | Certain |
| Scene map heap (town / interior / dungeon) | Shared scene pool | `g01` (`9.47` MB) / `i04h01a` (`3.75` MB) / `d02f01` (`6.91` MB) | Scene-dependent | Estimated |
| SPU2 music region (`BG_*`) | `0x78000` B | `BG_056` (`0x77E80` B) | `384` B | Certain |
| SPU2 field region (field + event + monster banks) | `0x10DC10` B | `s01` set `1600` + `EV_100_155` (`0x10E180` B) | `-1,392` B (overflows) | Certain size, script-traced |
| SPU2 system sound region (`SY_000`) | `0x1D4C0` B | `SY_000` (`0x1C6F0` B) | `3,536` B | Certain |
| SPU2 menu (`SP_*`) / player (`CH_*`) bank | `0x30D40` / `0x19020` B | `SP_009` (`0x30B30`) / `CH_047` (`0x18190`) | `528` / `3,728` B | Estimated bound |

## 1. The EE memory map

At boot, `MainLoop` (`src/mainloop.cpp`, assembly `0x192298`) allocates a master heap, `MainBuffer`, of `0x1A0000` quadwords (`27,262,976` B) at `0x40B100`. Each game mode partitions `MainBuffer`:

| Mode & Init Function | Fixed Allocations (in 16-Byte Quadwords) | Dynamic Scene Pool |
|---|---|---|
| Town & field (`EditInit`, `src/editloop.cpp`, `0x1AB320`) | Render packets (`2 × 10,000` + `2 × 35,000`), `ScriptBuffer__2` (`20,000`), tail `0x334D0` (`MenuBuffer__2` / `read_buffer` sharing `200,000` + `WorkBuffer` `10,000`). | `TotalDataBuff` (~`22,140,928` B): holds common town assets, texture tables (`SetTableBuffer(0x15E, 0xDD)`), and chained scene stacks `0..8`. |
| Dungeon (`memoryInit`, `src/dng_main.cpp:240`) | Render packets (`2 × 10,000` + `2 × 20,000` + `2 × 60,000`), `WorkBuffer` (`10,000`), texture tables (`SetTableBuffer(0x140, 0xAF)`), party (`140,016`), `BuffScriptData` (`20,000`), effects (`40,000`), `BuffReadData` (`200,000`) = `592,757` qwords total. | ~`17.8` MB (`InitDungeonMain`): holds common dungeon models/effects, then chains stack `1` (`BuffStageMain`, `src/dng_main.cpp:670`) through monster, NPC, and event stacks. |

`CScene::AssignStack` (`src/scene.cpp`) chains scene stacks `0..8` so each stack receives the unused remainder of the preceding stack:

| Scene Stack | Symbol | Contents |
|---|---|---|
| `0` | `ControlCharaBuff` | Active party characters (`GetCharaMemAllocSize()` quadwords, subdivided by `mem_table`; section 4). |
| `1` | `MainDataBuff` / `BuffStageMain` | Main outdoor town/field map (`EditMapJump`, `src/editloop.cpp:438`) or dungeon floor map (`src/dng_main.cpp:670`). |
| `2` | `MainCharaBuff` | Townspeople (NPCs) or floor monsters on the main map. |
| `3` | `SubDataBuff` | Interior sub-map or secondary Palm Brinks district (`LoadSubMap`, `src/mapjump.cpp:207`). |
| `4` | `SubCharaBuff` | Townspeople inside the sub-map. |
| `5..8` | `EventBuff` | Event characters and objects loaded via `load_chara(MEMORY_STACK_EVENT, ...)`. |

## 2. The read buffer and the script buffer

| Buffer | Capacity | Staging & Allocation Rules | Largest Asset on Disc | Headroom |
|---|---|---|---|---|
| `read_buffer` / `BuffReadData` (single file) | `0x30D40` qwords (`3,200,000` B) | Town `EditInit` sets `read_buffer_end = read_buffer + 0x30D400`; dungeon `dng_main.cpp:286` allocates `BuffReadData` (`200,000` qwords). Stages whole `.chr` packs (`GetLoadBGBuff` in `src/event_func.cpp:2383`, `src/charasetup.cpp`) and `.snd` banks. | `event/e600/e612/e612_griffon.chr` (`3,197,312` B; `e612_gri_kao.chr` at 6.2 MB is unused) | `2,688` B |
| `read_buffer` (town map load) | `0x2F9B80` B (`3,120,000` B) | `LoadMapData` (`src/sceneload.cpp`) reads primary + add-map (`map/cmn/`) `.map`, `.cfg`, `.mpk`, and `.sky` (or `map/def.sky` at `276,912` B), rounding each up to 64 B. Loading screen packets occupy the top `0x1388` qwords. | `s48` (`3,021,376` B) | `98,624` B |
| `BuffReadData` (dungeon map load) | `3,040,000` B | Same as town map load, but dungeon loading screen packets start at `3,040,000` B (`src/dng_main.cpp:291`). | `d05f02` (`2,744,384` B) | `295,616` B |
| `ScriptBuffer__2` / `BuffScriptData` | `312,816` B usable (`20,000` qwords = `320,000` B total) | `SetEventScript` (`src/event.cpp:106`) reserves `0x40 + 0x180` qwords (`7,168` B) for interpreter state tables before loading the map `.stb` (note: `dungeon/sys_script/*.stb` up to `361,160` B use a separate buffer). | `map/d/d05/d05_5.stb` (`309,664` B) | `3,152` B |

## 3. Maps

| Budget / Limit | Capacity | Engine Function & Details | Largest Use on Disc |
|---|---|---|---|
| Town / interior part placements | `320` (`0x140`) | `SetPlacePartsBuff` (`src/map.cpp:224`, `GetLoadMapInfo`, `LoadSubMap`); includes add-map placements. Excess returns `NULL` in `NewPlaceParts`. | `m08` (`230`), `m05` (`245`) |
| Dungeon part placements | `400` | `LoadDungeonMapFile` (`src/dng_event.cpp`). | `d06b02` (`102`) |
| Map table (`map/map<lang>.cfg`) | `0x8000` B (`32,768` B) | `MapNameBuff` (`src/mapselect.cpp`): `0x1C` B/row (+ sentinel) + string pool, each 16-byte aligned. | `9,248` B (JA) .. `13,808` B (KO) |
| Town map texture blocks | `24` (with interior) / `64` (alone) | `CMap::LoadData` (`src/map.cpp:1506`): blocks `0x00..0x17` when an interior loads at `0x18`, or `0x00..0x3F` when no interior loads (`s53`, `s10`, `s09` never load interiors). | `s53` (`32`), `s10` (`31`), `s09` (`27`) |
| Interior texture blocks | `40` (`0x18..0x3F`) | `LoadSubMap` (`src/mapjump.cpp`). | `i04h01a` (`16`) |
| Dungeon texture blocks | `15` (`0x00..0x0E`) | Dungeon map loader (effects at `0x0F`, party at `0x10`). | `d06f02`, `d06f03` (`14`) |

**Scene stack footprint (`CScene::LoadMapFromMemory` in `src/sceneload.cpp`, estimated)**:
- Copies `.ipk` and `.efp` archives (64-byte aligned), sky packs (`CMapSky::LoadPack` in `src/mapsky.cpp`), and per-placement `CMapParts::Copy` visuals (`src/mapparts.cpp:803`: `0xD0` B/piece + materials).
- Parses `.pcp` models by `TYPE` in `info.cfg` (`CMdsList::LoadPCPFile`, `src/mdslist.cpp:484`):
  - `TYPE 0` (Static visual, `mgLoadMDSFile` in `src/mg_dataset.cpp:502`): `0x110` B/frame, name, attr (`0x90`), bounds (`0xB0`), `0x50`-byte `mgCVisualFixMDT`, `0x30` B/material, and compiled VU1 packets (`CreateFacePacket`).
  - `TYPE 1` / `2` (Collision, `LoadCollisionFile` in `src/collision.cpp:387`): `0x120` B/frame + `0x50` B/triangle (strips/coloured prims ignored).
  - `TYPE 3` / `4`: Animated character model (section 4).
- **Largest estimated map footprints** (categorised by `map/map<lang>.cfg`: types `7, 8` = dungeon; types `2, 4, 6` and `m02`–`m05` = `SubDataBuff`; others = `MainDataBuff`):
  - Town main map (`MainDataBuff`): `g01` at **`9,471,152` B** (`9.47` MB)
  - Interior sub-map (`SubDataBuff`): `i04h01a` at **`3,752,880` B** (`3.75` MB)
  - Dungeon map (`BuffStageMain`): `d02f01` at **`6,911,504` B** (`6.91` MB)

## 4. Character packs

`CCharacter2::LoadPack` (`ScanInfoFile`, `src/character.cpp:1286`) allocates:
- 64-byte-aligned `IMG` copies (`stAlloc64(size / 16 + 1)`).
- Main `MODEL` (`.mds`), with `VERTEX_ANIME` meshes as `mgCVisualMotionMDT` (`0x110` B + VU1 packets with 2 weight qwords/vertex in batches of `((180 - 2 * bones - 2) / 5 / 3) * 3` vertices; `src/visualmotion.cpp`) and `SHADOW_MODEL` vertex/index copies.
- Each `MOTION` set's `.bbp` array and `.mot` track lists (`CreateAnimeDataEX` in `src/gameutil.cpp:807`: `0x20` B/track, `16` B/key, `4` B/frame number), plus `KEY` clips (`0x30` B each), outlines, sound cues, and `.clo` simulators.

**Party character stacks** (`GetCharaMemAllocPtr` in `src/charasetup.cpp:248`, `mem_table` at `0x351D50` in 16-byte quadwords; `SetupMainUnit` at `src/charasetup.cpp:100`):

| `mem_table` Row / Metric | Stack `0` (Base/Motions) | Stack `1` (Costume) | Stack `2` (Melee Weapon) | Stack `3` (Ranged Weapon) | Stack `4` (Hat/Accessory) | Stack `5` (Shoes) | Stack `6` (Action Script) |
|---|---|---|---|---|---|---|---|
| `0`: Max / Monica (dungeon) | `68,500` (`1,096,000` B) | `40,000` (`640,000` B) | `7,000` (`112,000` B) | `4,650` (`74,400` B) | `3,000` (`48,000` B) | `9,900` (`158,400` B) | `4,500` (`72,000` B) |
| `1`: Ridepod | `30,000` (`480,000` B) | `30,000` (`480,000` B) | `60,000` (`960,000` B) | `10,000` (`160,000` B) | `10,000` (`160,000` B) | — | — |
| `2`: Max / Monica (town) | `75,000` (`1,200,000` B) | `40,000` (`640,000` B) | `7,000` (`112,000` B) | `4,650` (`74,400` B) | `3,000` (`48,000` B) | `9,900` (`158,400` B) | — |
| `3`: Monster Badge form | `120,000` (`1,920,000` B) | — | — | — | — | `5,500` (`88,000` B) | — |
| **Peak dungeon pack use** | `c01_base3`: `1,080,672` B (~`15` KB free; Monica `958,576` B; town `664,976` B; unused `c01_base0` is `1,105,632` B) | `641,296` B (`LoadPack` est.; `LoadSkin` uses less) | `ws*`: `104,496` B (`7,504` B free) | `wg*`: `71,968` B (`2,432` B free) | `ha*`: `47,792` B (`208` B free) | `157,616` B (`784` B free) | — |

- **Single skinned mesh limits (certain)**:
  - **Max `3,198` vertices per skinned mesh**: `_MODEL` (`src/character.cpp:1582`) builds the temporary weight table (`32` B/vertex + `2` header qwords) inside a fixed `6,400`-quadword (`102,400`-byte) work buffer.
  - **Max `32` bones per VU1-skinned mesh**: Enforced in `CreateVertexWeight`.

## 5. Texture memory (GS)

- **GS VRAM per logical block (`4,768` GS blocks = `1,220,608` B, certain)**:
  - Spans from `vram_top = 0x2D20` (`GetVramTopAddress` in `src/mainloop.cpp:101`: `0x2D00` after 512×480 32-bit frame/Z buffers from `mgInit(2, 3)` + `0x20`) to `vram_fix = 0x3FE0` (`Initialize`) minus `32` GS blocks reserved for photography in `LoadTakePhoto` (`src/photo.cpp:61`).
  - `GetRemainVRAM` (`src/mg_texture.cpp:452`) rounds each texture up to 32-block GS pages (`8,192` B; each mip level uses `bpp * w * h / 2048` blocks, with 24-bit counted as 32-bit) plus `4` GS blocks (`1,024` B) per CLUT.
  - Single-level 8-bit textures with `w * h <= 65,536` unswizzle in place (`Conv32To8`) and cache in unused Z-buffer VRAM at 4× size while the `4,096`-unit pool lasts (`GetZBufVram`, `CheckCopyToZBufVram`).
  - When `GetRemainVRAM < 0`, `EnterIMGFile` (`src/mg_texture.cpp:753`) prints `"texture over"` and spills into the next logical block (`s01`'s fullest block leaves **12 GS blocks**; maps `f04`, `s01`, `s28`, `s29`, `s30`, `s51`, `s53`, `s55`, and `s67` spill internally).

**Logical texture block assignments**:

| Owner Category | Town Mode (`SetTableBuffer(0x15E, 0xDD)`: 350 tex, 221 blocks) | Dungeon Mode (`SetTableBuffer(0x140, 0xAF)`: 320 tex, 175 blocks) |
|---|---|---|
| Main / floor map + add-map | `0x00..0x17` (`24` blocks; `0x00..0x3F` when no interior loads) | `0x00..0x0E` (`15` blocks) |
| Interior sub-map | `0x18..0x3F` (`40` blocks) | — |
| Map / interior effects | `0x40` / `0x41` | `0x0F` |
| Active party | `0x46..0x4D` (`8` blocks) | `0x10..0x17` (`8` blocks) |
| Main map townspeople | `0x4E..0x5D` (`16` blocks) | `0x18..0x27` (shared with event characters) |
| Interior townspeople | `0x5E..0x65` (`8` blocks) | — |
| Event characters / monsters | `0x66..0x85` (`32` blocks, event characters) | `0x28..0x33` (`1` block per monster species) |
| Menus & font | `0x86..` (menus), `0x9A` (font) | — |
| Sky (`.sky`, 4 `.img` files) | `0xA9..0xAC` (`4` blocks, 1 per `.img`) | `0x4C..0x4F` (`4` blocks, 1 per `.img`) |

## 6. Sound memory (SPU2)

Each loaded `.snd` bank occupies `bd_size + 16` bytes in the 2 MB SPU2 RAM (`sndLoadSound` -> `LoadHdBd` / `LoadHdBdAdd` at `0x18B9F8`; base addresses set in `CSound::Init`, `src/sound.cpp:271-283`). Reverb mode `4` (`Hall`) reserves `0x6FE0` B at the top of Core 1 (`0x1D9020..0x1DFFFF`) and Core 0 (`0x1F9020..0x1FFFFF`, `src/sound.cpp:45, 73`).

| SPU2 Region | Base & Direction | Bound | Capacity (Bytes) | Largest Use on Disc | Status & Headroom |
|---|---|---|---|---|---|
| Music (`BG_*`, ports `0`, `11`) | `0x005210` up | `0x07D210` | `0x78000` (`491,520`) | `BG_056` (`0x77E80`) | Certain (`384` B free) |
| Field (`BS`, `FG`, `SR`, `<=16 OB`, `1 EV`) | `0x07D210` up | Monster banks (`0x18AE20` down) | `0x10DC10` (`1,104,912`) combined | `s01` set `1600` + `EV_100_155` (`0x10E180`) | Certain (`-1,392` B overflow) |
| Monsters (`EN_*`, port `5`) | `0x18AE20` down | Top of field banks | Shares `0x10DC10` pool | Up to 6 banks (`0x2B2D0` max/floor) | Certain (`>= 343,824` B free on dungeon floors) |
| System (`SY_000`, port `6`) | `0x18AE20` up | `0x1A82E0` | `0x1D4C0` (`120,000`) | `SY_000` (`0x1C6F0`) | Certain (`3,536` B free) |
| Menu (`SP_*`, port `8`) | `0x1A82E0` up | Core 1 reverb (`0x1D9020`) | `0x30D40` (`200,000`) | `SP_009` (`0x30B30`) | Estimated (`528` B free) |
| Player (`CH_*`, port `7`) | `0x1E0000` up | Core 0 reverb (`0x1F9020`) | `0x19020` (`102,432`) | `CH_047` (`0x18190`) | Estimated (`3,728` B free) |

- **Field and event stacking (`CScene::LoadSound`, `src/scenesnd.cpp:365`)**:
  - Stacks `BS` -> `FG` -> `SR` -> `OB` in order; reloading any layer resets all later layers.
  - `CScene::LoadSound` **ignores** the sound set's event column; an `EV_*` bank loads onto port `4` (at the top of the active field stack) only when a script calls `load_sound(set)` (`GetDefEventSeFile`).
  - In `s01` (`play_blackstone_railroad_arrival_scene`), loading `EV_100_155` (`0x83E30` B) on top of field set `1600` (`0x8A350` B) reaches `0x18B390`, overflowing `0x18AE20` by **`1,392` bytes** (`"SYS AREA HAKAI"`) and overwriting the start of `SY_000.snd`. The second largest script combination (`d03b02` set `3691` + `EV_115_158`) leaves `5,792` B free.
- **IOP transfer buffer (`src/sound.cpp:122-159`)**: Stages `.bd` uploads through a `0x6DD00`-byte IOP buffer (`<= 0x6DE00` B in 1 chunk, `> 0x6DE00` B up to `0xDBA00` B in 2 chunks; avoid `.bd` sizes in `0x6DD01..0x6DE00`). Largest on disc is `EV_130_150.snd` (`0xD2B10` B).
- **EE sound stacks**: See [the sound bank format](sounds.md#8-memory-the-game-gives-a-bank).
