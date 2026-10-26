# DC2 sound banks (`.snd`)

This page documents how the PAL disc stores sound effects inside `.snd` archives and how `play_se(bank, n)` triggers SPU2 hardware voices.
- For replacing and adding sound effects in a mod, see [editing sounds](../guide/sounds.md).
- For the `dc2tk` sound converter implementation, see [src/docs/assets/sounds.md](../../src/docs/assets/sounds.md).
- For background music and ambient loop sequences inside sound banks, see [music](music.md).

**Sources**:
- **Engine**: Decompiled EE code in `finaldc2/ChronicleTwo/ps2` (`src/snd_mngr.cpp`, `src/sound.cpp`, `src/snd_seseq.cpp`, `src/scenesnd.cpp`, assembly of `LoadSeInfoTxt`, `LoadVolInfoTxt`, `GetLine`, `SearchSeq`, `sndLoadSound`, `sndSePlaySeID`, `sndCSeSeq::NoteOn`, and notes in `re/docs/snd_mngr` and `re/docs/snd_seseq`).
- **Data**: Corpus counts across all 861 `.snd` banks on the PAL disc (`extracted/disc/**/NAME.snd`, including 20 duplicate copies in menu and fishing packs outside `snd2/`).
- **Sony SDK**: `.hd` uses Sony's standard hardware-synth bank format (`sceHardSynth*` headers), parsed on the IOP by `modhsyn` through the EZMIDI driver.

## 1. The `.snd` pack

A `.snd` archive is a 16-byte-aligned chain pack (see [the disc](disc.md)):

| Offset | Size | Type | Field | Description |
|---|---|---|---|---|
| `0x00` | `64` | `char[64]` | `name` | NUL-terminated CP932 filename (bytes after NUL contain leftover exporter memory). |
| `0x40` | `4` | `u32` | `offset` | Payload offset from slot start (always `0x50`). |
| `0x44` | `4` | `u32` | `size` | Payload byte size. |
| `0x48` | `4` | `u32` | `next` | Offset to the next slot header (`0x50 + align16(size)`). |
| `0x4C` | `4` | `u32` | `unk_4c` | Unused word (holds the same leftover value across all slots in a pack). |
| `0x50` | `size` | `u8[]` | `data` | Member payload, zero-padded to 16 bytes. |

The chain ends with a `0x50`-byte terminator slot (`name[0] == 0x00`, words `50 00 00 00 ff ff ff ff ff ff ff ff xx xx xx xx`). `sndLoadSound` finds members by extension case-insensitively via `GetPackFileExt`. Pack order affects only the indexing order of `.sq` and `.mid` members.

| Extension | Count/Bank | Corpus Total | Purpose |
|---|---|---|---|
| `cfg` | `1` (required) | `861` | Sound-effect entry table. |
| `vol` | `1` (required) | `861` | Default per-entry volumes and port reverb settings. |
| `hd` | `1` | `861` | Instrument and sample header, uploaded to the IOP heap. |
| `bd` | `1` | `861` | ADPCM sample data body, DMA-transferred to SPU2 RAM. |
| `sq` | `0..8` | `447` | Sony MIDI sequences (see [music](music.md)). Loaded on game ports `0`, `2`, `4`, `11` (max 32 names on EE; max 10 sequences per IOP port via `MIDI_PORT_SEQ_MAX`). |
| `mid` | `0..48` | `2,146` | Standard MIDI File (SMF) sound-effect sequences, converted at load time. |

### Which copy the game reads

`GetLoadBGBuff` resolves file paths in three steps: (1) an active background load (`start_background_load`), (2) a preloaded pack (`load_pack_file`, unused on the disc), and (3) `LoadFile2` (`SearchFile` in `DATA.HD4` -> `CDRead` from `DATA.DAT`).

| Bank Category | Loaded Path | Loader Function | Ignored or Divergent Duplicates |
|---|---|---|---|
| Event (`EV_*`) | `snd2/event/EV_<a>_<b>.snd` | `_SND_LOAD_SOUND`, `GetDefEventSeFile` (`at_1132__4`) | Always read from `DATA.DAT` (HDD paths stubbed out; `LoadFile2` RAM cache holds only villagers). |
| System (`SY_000`) | `snd2/SY_000.snd` (port `6`) | `MainLoop` (`at_1212`) | `snd2/system/SY_000.snd` is an older, unreferenced copy. |
| Field (`BS`, `FG`, `SR`, `OB`) | `snd2/bs/BS_*.snd`, `snd2/fg/FG_*.snd`, `snd2/env/SR_*.snd`, `snd2/ob/OB_*.snd` | `GetSe*File` (`src/scenesnd.cpp`) | None. |
| Menu (`SP_*`) | `snd2/sp/SP_*.snd` and `menu/*/*.pac/SP_*.snd` | Scene and menu loaders | Copies of `SP_001`..`SP_003` inside menu `.pac` files have different `.vol`/`.hd` data from `snd2/sp/`. |
| Fishing music (`BG_193`) | `sg/fish/fishing2*.pak/BG_193.snd` | Fishing minigame loader | Differs from `snd2/bgm/BG_193.snd`. |

`SOUND.DAT` / `SOUND.HD3` store streamed `.wav` voice files, not `.snd` banks. `DATA.HD3` is unused.

## 2. From `play_se(bank, n)` to voices

`sndLoadSound(port, pack)` assigns a sequential slot `bank` (`0..15`) on `port` and returns `sound_id = (port << 24) | (bank << 16)`. Calling `play_se(id, n)` invokes `sndSePlaySeID(id, n, velocity = -1, vol = -1, pan = 0x40, pitch = 0x2000, voice)`, which reads entry `n` from the runtime `sndSeInfo` table (`0x0C` bytes per entry; substitutes `def_vol` from `.vol` when `vol < 0`):

| `sndSeInfo.type` | Source in `.cfg` | Runtime Action | Corpus Entries (`16,127` total) |
|---|---|---|---|
| `0` (None) | Missing `.sq` / `.mid` | Does nothing (silent). | `254` |
| `1` (`KeyOn`) | `"KeyOn"`, `"-"`, etc. | Calls `CSound::SE_Play(port, bank, prog, key, ...)`, sending MIDI `Bn 00 bank` (Bank Select), `Cn prog` (Program Change), two `F9` messages (vol/pan, pitch), and `FD 10 key id velocity` (Key-On, `velocity = -1` -> `127`). | `13,284` (`3,781` playable; `9,503` silent placeholders) |
| `2` (`sq`) | Matched `.sq` name | Calls `SQ_Play(sq_port, prog = sq_index, vol)`. | `447` |
| `3` (`seseq`) | Matched `.mid` name | Calls `PlaySeSeq` on the EE. Each note-on calls `SE_Play` with the track's program, note key, and the **note's own velocity** (`sndCSeSeq::NoteOn`). | `2,142` |

**IOP voice lookup chain**:
```
Prog[prog] -> splits where range_low <= key <= range_high
           -> Sset[split.sample_set] (vel_limit_low..vel_limit_high)
           -> Smpl[i] where vel_range_low <= velocity <= vel_range_high
           -> Vagi[sample.vag_index] -> .bd byte offset, sample rate, loop flag
```

- **Silent `KeyOn` breakdown (`9,503` rows)**: `9,236` reference an absent program (`0xFFFFFFFF`), `256` have no split covering `key`, and `11` reference `vag_index == 0xFFFF`.
- **Voice and sample counts**: `4,262` entries trigger 1 voice; `1,661` trigger `>= 2` voices. Across `7,397` total VAG samples on the disc, `1,531` are shared by multiple `.cfg` rows, `2,009` (in `snd2/`) are used only by `.sq`/`.mid` sequences, and `310` are unreferenced.

## 3. `.cfg` (sound-effect table)

CP932 text file with `\r\n` (782 banks) or `\r` (79 banks) line endings (`GetLine` accepts `\r\n`, `\r`, or `\n` and splits on tabs or single spaces). `LoadSeInfoTxt` makes two passes:
1. Counts all lines before `END` (including the header line) to allocate `se_num` entries.
2. Parses lines: `END` stops; `REVERBE type depth` sets SPU2 reverb (`Room`..`Max` = `1..10`, unused in `.cfg`); any line starting with `'0'..'9'` fills the **next sequential slot** (column 0's numeric value is ignored); other lines are skipped.

| Column | Stack Buffer | Content |
|---|---|---|
| `0` | `8` B | Row number (human reference only; ignored by parser). |
| `1` | `64` B | Category label in Japanese (`-` on placeholder rows). |
| `2` | `64` B | Sound name in Japanese. |
| `3` | `8` B | `0` or `*` (never read). |
| `4` | `64` B | Trigger string passed to `SearchSeq(col4)` (see below). |
| `5` | `8` B | Program number (`s8`, masked to `0..127` by `SE_Play`); overwritten with sequence index on `.sq`/`.mid` rows. |
| `6` | `8` B | MIDI key (`s8`; `60` in 15,581 rows; `62`, `49`, `35`, `53`, `58` also occur). |
| `7` | `12` B | `○` or empty -> stored in `sndSeInfo.flag` (never read). |

> [!CAUTION]
> `LoadSeInfoTxt` copies each column into its stack buffer without bounds checking. Exceeding `63` bytes (names) or `7` bytes (numbers) overflows the EE stack.

- **`SearchSeq(col4)` resolution**:
  - `"KeyOn"` -> type `1` (`KeyOn`)
  - Case-insensitive match to loaded `.sq` -> type `2` (`sq`), `prog = sq_index`
  - Case-insensitive match to loaded `.mid` -> type `3` (`seseq`), `prog = mid_index`
  - Any other string containing `.` -> type `0` (silent)
  - Anything else (`KeyON`, `Keyon`, `-`, empty) -> type `1` (`KeyOn`)

## 4. `.vol` (default volumes)

ASCII text file with `\n` (725 banks), `\r\n` (135), or `\r` (1) line endings (`LoadVolInfoTxt`):
- `END`: Stops parsing.
- `REVERB type depth` (79 banks, e.g. `REVERB 4 32`): Calls `SetReverb` on SPU2 Core 0 (driver port `0`) or Core 1 (driver port `7`).
- `<index> <vol>`: Sets `se[index].def_vol = vol` if `index < se_num` (unlisted entries default to `0x40` = `64`). Because `se_num` includes the `.cfg` header line, 851 banks include 1 extra dummy volume row at the end.

## 5. `.hd` (instrument header)

Little-endian Sony hardware-synth bank containing six chunks in fixed order: `Vers`, `Head`, `Prog`, `Sset`, `Smpl`, `Vagi`. All 861 `.hd` files have zero gaps and no shared record offsets.

**Common chunk header**:

| Offset | Size | Type | Field | Description |
|---|---|---|---|---|
| `0x00` | `4` | `char[4]` | `creator` | `"IECS"` (`"SCEI"` byte-reversed). |
| `0x04` | `4` | `char[4]` | `type` | `"sreV"`, `"daeH"`, `"gorP"`, `"tesS"`, `"lpmS"`, or `"igaV"`. |
| `0x08` | `4` | `u32` | `chunk_size` | Total chunk byte size including header and `0xFF` alignment padding. |
| `0x0C` | `4` | `u32` | `max_index` | Present in `Prog`, `Sset`, `Smpl`, `Vagi`: `count - 1`. |
| `0x10` | `4*count` | `u32[]` | `offsets` | Present in `Prog`, `Sset`, `Smpl`, `Vagi`: record offset from chunk start (`0xFFFFFFFF` = absent). Records follow in ascending index order, padded with `0xFF` to 16 bytes. |

### 5.1 `Vers` (`0x10` bytes, at `0x00`) and `Head` (`0x40` bytes, at `0x10`)

| Chunk | Offset | Size | Type | Field | Value / Description |
|---|---|---|---|---|---|
| `Vers` | `0x0C` | `2` | `u16` | `reserved` | `0` |
| `Vers` | `0x0E` | `1` | `u8` | `ver_major` | `2` |
| `Vers` | `0x0F` | `1` | `u8` | `ver_minor` | `0` |
| `Head` | `0x0C` | `4` | `u32` | `hd_size` | Total `.hd` byte size |
| `Head` | `0x10` | `4` | `u32` | `bd_size` | Total `.bd` byte size |
| `Head` | `0x14` | `16` | `u32[4]` | `chunk_ofs` | File offsets of `Prog`, `Sset`, `Smpl`, and `Vagi` chunks |
| `Head` | `0x24` | `4` | `u32` | `se_timbre` | `0xFFFFFFFF` (none) |
| `Head` | `0x28` | `24` | `u32[6]` | `reserved` | All `0xFFFFFFFF` |

### 5.2 `Vagi` (VAG sample info, `0x08` bytes per record, chunk at `0x50`)

No `Vagi` table contains `0xFFFFFFFF` entries. Sample byte length is `next_offset - offset` (or to the end of `.bd`).

| Offset | Size | Type | Field | Corpus Values |
|---|---|---|---|---|
| `0x00` | `4` | `u32` | `bd_offset` | Byte offset in `.bd` (ascending from `0`, contiguous, 16-byte aligned). |
| `0x04` | `2` | `u16` | `sample_rate` | Hz: `28000` (2,765), `32000` (1,520), `44100` (652), `24000`, `44066`, `22050`, etc. |
| `0x06` | `1` | `u8` | `loop` | `1` = looped (2,106), `0` = one-shot (5,291). Matches `.bd` ADPCM flags in all 7,397 samples. |
| `0x07` | `1` | `u8` | `reserved` | Always `0xFF`. |

### 5.3 `Smpl` (Sony `SampleParam`, `0x2A` bytes per record)

| Offset | Size | Type | Field | Typical Corpus Values |
|---|---|---|---|---|
| `0x00` | `2` | `u16` | `vag_index` | Index into `Vagi` (`0xFFFF` = silent, in 111 samples across 35 banks). |
| `0x02` | `3` | `u8[3]` | `vel_low`, `vel_xfade`, `vel_high` | `1, 64, 127` (or velocity-split subranges). |
| `0x05` | `3` | `s8, u8, u8` | `vel_pitch`, `centre`, `curve` | `0, 1, 0` |
| `0x08` | `3` | `s8, u8, u8` | `vel_amp`, `centre`, `curve` | `0, 1, 0` (curve `2` in 19 samples). |
| `0x0B` | `2` | `u8, s8` | `base_note`, `detune` | `60` (`48`, `72`, etc.), `0` |
| `0x0D` | `4` | `u8[4]` | `pan`, `group`, `prio`, `vol` | `64` (centre), `0..112`, `10`/`100`, `128` |
| `0x11` | `1` | `u8` | `reserved` | `0xFF` |
| `0x12` | `4` | `u16[2]` | `adsr1`, `adsr2` | Raw SPU2 envelope registers (`0x80FF`, `0x5FEC`). |
| `0x16` | `10` | `(s8, u8)[5]` | `key_follow_adsr` | `AR`, `DR`, `SR`, `RR`, `SL` and centre notes (`0, 60`). |
| `0x20` | `8` | `u16[4]` | `lfo_timing` | Pitch LFO delay/fade, amp LFO delay/fade (`0`). |
| `0x28` | `2` | `u8[2]` | `lfo_attr`, `spu_attr` | `0`/`1`/`16`/`17`, `0x2F` (`0x1F`, `0x23`, etc.). |

### 5.4 `Sset` (sample sets, `4 + 2 * n` bytes per record, unaligned)

| Offset | Size | Type | Field | Corpus Values |
|---|---|---|---|---|
| `0x00` | `1` | `u8` | `vel_curve` | `0` or `2` |
| `0x01` | `2` | `u8[2]` | `vel_limit_low`, `vel_limit_high` | `1`, `127` |
| `0x03` | `1` | `u8` | `sample_count` (`n`) | `1` (12,868 sets), `4` (165), `6` (116), `13` (21), `2` (3). |
| `0x04` | `2*n` | `u16[n]` | `sample_indices` | Indices into `Smpl` (multi-sample sets partition velocity; only `.mid`/`.sq` use velocities `< 127`). |

### 5.5 `Prog` (Sony `ProgramParam`, `0x24 + n * 0x14` bytes per record)

Up to 128 programs per bank (`FG_000.snd` has 128); 506 banks contain absent (`0xFFFFFFFF`) program slots.

| Struct | Offset | Size | Type | Field | Typical Corpus Values |
|---|---|---|---|---|---|
| `ProgramParam` | `0x00` | `4` | `u32` | `split_ofs` | Always `0x24`. |
| `ProgramParam` | `0x04` | `2` | `u8[2]` | `split_count` (`n`), `split_size` | `n = 1` (4,891), `2` (1,119), `3` (461), up to `57`; `split_size = 0x14`. |
| `ProgramParam` | `0x06` | `4` | `u8, u8, s8, s8` | `vol`, `pan`, `transpose`, `detune` | `128, 64, 0, 0` |
| `ProgramParam` | `0x0A` | `4` | `s8, u8, u8, u8` | `key_pan`, `centre`, `attr`, `reserved` | `0, 60, 0` (`1` in 29 progs), `0xFF` |
| `ProgramParam` | `0x0E` | `22` | LFO params | Waveforms, phases, periods, pitch/amp depths | Standard LFO setup. |
| `SplitBlock` | `0x00` | `2` | `u16` | `sample_set` | Index into `Sset`. |
| `SplitBlock` | `0x02` | `4` | `u8[4]` | `key_low`, `key_xfade`, `key_high`, `idx` | Key range (`12..119`, etc.) and split index `0..n-1`. |
| `SplitBlock` | `0x06` | `4` | `u16[2]` | `bend_low`, `bend_high` | Pitch-bend range (`256` = 1 semitone). |
| `SplitBlock` | `0x0A` | `6` | `(s8, u8)[3]` | `key_follow` | Pitch, amp, and pan key-follow + centre notes (`0, 60`). |
| `SplitBlock` | `0x10` | `4` | `u8, u8, s8, s8` | `vol`, `pan`, `transpose`, `detune` | `128, 64, 0, 0` |

## 6. `.bd` (samples): PS2 SPU2 ADPCM

Raw SPU2 ADPCM streams concatenated back-to-back (16-byte aligned, no header). Each 16-byte block decodes to 28 16-bit PCM samples:

| Byte | Content |
|---|---|
| `0` | Low nibble: `shift` (`0..12`, `1..12` on disc). High nibble: predictor `filter` (`0..4`). |
| `1` | Flags: bit 0 (`1`) = loop end, bit 1 (`2`) = loop repeat, bit 2 (`4`) = loop start. |
| `2..15` | 28 signed 4-bit nibbles, **low nibble first**: `s = clamp(((nibble << 12) >> shift) + ((h1 * c1 + h2 * c2 + 32) >> 6), -32768, 32767)`. |

| Filter | `0` | `1` | `2` | `3` | `4` |
|---|---|---|---|---|---|
| `(c1, c2)` | `(0, 0)` | `(60, 0)` | `(115, -52)` | `(98, -55)` | `(122, -60)` |

**Block flag patterns across all 7,397 VAG streams** (block `0` is always 16 zero bytes of lead-in silence):

| Sample Type | Block Flag Sequence | Corpus Count | Hardware Behaviour |
|---|---|---|---|
| One-shot | `0  4  0+  1  7` (or `0  5  7`) | `5,273` (`+ 18`) | First data block sets loop address (`4`); last block sets end (`1`, jumps to loop address and mutes), followed by unplayed `00 07 77..77` terminator. |
| Looped | `0  2+  6  2+  3` (or `0  6  2+  3`) | `2,077` (`+ 29`) | Data blocks set repeat (`2`); loop start sets `6` (`4\|2`); final block sets `3` (`2\|1`, jumps to loop start without muting). `loop_start = (loop_block - 1) * 28` samples. |

## 7. `.sq` and `.mid`

- **`.sq` (Sony sequence)**: Background music (`BG_*`), ambient loops (`SR_*`), and 7 event sequences (`EV_*`). See [music](music.md).
- **`.mid` (Standard MIDI File)**: All 2,146 `.mid` members are Format 0, single-track SMF files with division `480`. `sndCSeSeqData::LoadSMF` converts each file into 6-byte engine events:
  - Supported events: Note On, Note Off, Program Change, Pitch Bend, and CC `7` (Volume), `10` (Pan), `11` (Expression), `110` (Loop: `0` = start, `127` = jump to start).
  - MIDI channels map to engine tracks `0..7` (channel 0 holds only CC `110` loop markers; tracks init to `prog = 0`, `vol = 127`, `expr = 127`, `pan = 64`).

## 8. Memory the game gives a bank

### 8.1 SPU2 RAM (`CSound::Init`, `LoadHdBd2`)

Each loaded bank consumes `bd_size + 16` bytes in SPU2 RAM. Crossing `0x18AE20` from the field region prints `"SYS AREA HAKAI"`.

| Game Port | Bank Category | Driver Port | SPU2 Base | Growth Direction and Bound |
|---|---|---|---|---|
| `0` / `11` | Music (`BG_*`) | `0` / `3` | `0x005210` | Upward to `0x07D210` (`0x78000` B; `BG_056` uses `0x77E80`). |
| `3, 9, 2, 1, 4` | `BS`, `FG`, `SR`, `OB`, `EV` | `10, 8, 1, 15, 14` | `0x07D210` | Upward, stacked in listed order (reloading an earlier layer resets later layers). |
| `5` | Monsters (`EN_*`) | `13` | `0x18AE20` | Downward into the top of the field region. |
| `6` | System (`SY_000`) | `12` | `0x18AE20` | Upward to `0x1A82E0` (`0x1D4C0` B). |
| `8` | Menu (`SP_*`) | `11` | `0x1A82E0` | Upward to Core 1 reverb work area (`0x1D9020`). |
| `7` | Player (`CH_*`) | `9` | `0x1E0000` | Upward to Core 0 reverb work area (`0x1F9020`). |
| `10` | Unused | `7` | `0x18AE20` | Upward (never loaded). |

### 8.2 EE memory stack, IOP heap, and read buffer

In `sndLoadSound`, a bank allocates `16 * (ceil(se_num * 12 / 16) + 2)` bytes for `sndSeInfo` (12 bytes per `.cfg` row), plus `.sq` name tables and converted 6-byte `.mid` event streams (`src/scenesnd.cpp`):

| Allocation Target | Budget / Formula | Largest Use on Disc |
|---|---|---|
| EE stack: BGM (`BG_*`) | `0x400` B (`1,024` B) | `BG_031` (`160` B) |
| EE stack: Ambient (`SR_*`) | `0x400` B (`1,024` B) | `SR_120` (`368` B) |
| EE stack: Objects (`OB_*`, shared) | `0x400` B (`1,024` B) | `OB_000` (`176` B) |
| EE stack: Base (`BS_*`) | `0x2000` B (`8,192` B) | `BS_999` (`4,576` B) |
| EE stack: Battle (`FG_*`) | `0x2000` B (`8,192` B) | `FG_000` (`7,072` B) |
| EE stack: Player (`CH_*`) | Character's own memory stack (`src/dng_main.cpp`) | — |
| IOP heap (`.hd`) | `max(256, hd_size + 256)` B | — |
| EE `read_buffer` (whole `.snd`) | `3,200,000` B (see [memory](memory.md)) | — |

## 9. Open problems

1. **Sony `.hd` parameter nuances**: Velocity cross-fades, pitch-bend ranges, velocity curves, LFO depths, and SPU attribute bits follow Sony SDK documentation rather than hardware measurement.
2. **ADPCM rounding**: Exact 1-LSB hardware rounding and shift values `13..15` (unused on the disc) were not hardware-tested.
3. **Monster and event EE stacks**: The specific EE stack used for `EN_*` and `EV_*` `sndSeInfo` tables has not been traced (for SPU2 budgets, see [memory](memory.md)).
