# DC2 texture archives (`.img`: IM3, IM2, IMG)

A `.img` file is an archive of **standard TIM2 images** preceded by a name table. At runtime, `mgCTextureManager::EnterIMGFile` loads an entire `.img` archive and registers each picture under its name inside a texture block. Engine code, 3D models, and animation scripts look up textures **by name**. Three format versions exist: `IM3`, `IM2`, and `IMG`. Almost all archives on the disc use `IM3`.

- For editing textures as PNGs, replacing entries, and adding custom textures, see [editing textures](../guide/textures.md).
- For the `dc2tk` texture converter implementation, see [src/docs/assets/textures.md](../../src/docs/assets/textures.md).
- All multi-byte numbers in `.img` and TIM2 files are little-endian.

**Engine sources**:
- Struct declarations: `ps2/include/mg_texture.hpp` (lines 54–174)
- Loader and manager logic: `ps2/src/mg_texture.cpp` (`EnterTexture` lines 483–750, `EnterIMGFile` lines 754–900, `GetIMGVersion` / `mgGetIMGHeaderNum` / `mgGetIMGHeader` lines 910–1011, `ReloadTexture` lines 1189–1290, `ReloadCLUT` lines 1303–1325, `BlockConv32to8` / `PageConv32to8` / `Conv32To8` lines 1427–1580)
- Swizzle lookup tables: `ps2/asm/pal/data/mg_texture.data.s` (`lut_1246`, `block_table8_1266`, `block_table32_1267`)

## 1. Corpus at a glance

Counts across all 5,217 `*.img` files extracted from the PAL disc (`extracted/disc/`):

| Category | Count |
|---|---|
| Total `.img` files | `5,217` |
| Magic `IM3\0` | `5,155` |
| Magic `IM2\0` | `58` |
| Magic `IMG\0` | `3` |
| Empty file (`menu/2/op_bg.img`, 0 bytes) | `1` |
| Total entries (all versions) | `16,406` |
| Texture animation scripts (`#texanime`) | `1,178` |
| TIM2 pictures | `15,228` |
| Header-only pictures (`no_image = 1`) | `228` (`224` `PSMCT32`, `4` `PSMCT24`) |
| Pictures with pixel data | `15,000` |
| `PSMT8` (8-bit indexed) | `14,402` (`31` have 3 mip levels) |
| `PSMCT32` (32-bit RGBA) | `310` |
| `PSMT4` (4-bit indexed) | `278` |
| `PSMCT24` (24-bit RGB) | `10` |
| `PSMCT16` (16-bit RGBA) | `0` (supported by the engine, unused on the disc) |

> [!NOTE]
> The 209 `.map` files that start with `"IMG "` (with a trailing space) are text map files, not binary `.img` texture archives.

## 2. Archive header (`mgIMG_FILE_HEADER`, `0x10` bytes)

| Offset | Size | Type | `IM3` | `IM2` / `IMG` |
|---|---|---|---|---|
| `0x00` | `4` | `char[4]` | `"IM3\0"` | `"IM2\0"` / `"IMG\0"` |
| `0x04` | `4` | `u32` | Always `0x10` (header byte size; never read) | **Entry count** |
| `0x08` | `4` | `u32` | **Entry count** | `0` (`0x40` in `c14a01.img`) |
| `0x0C` | `4` | `u32` | `0` | `0` (`0x21` in `c14a01.img`) |

- `GetIMGVersion` compares only the first 3 bytes of the magic string.
- `mgGetIMGHeaderNum` reads the entry count from `0x04` for `IMG`/`IM2` and from `0x08` for `IM3`.
- The entry table starts at `0x10`. The first TIM2 picture or script starts immediately after the entry table (16-byte aligned in all 5,214 non-empty archives with entries).

## 3. Entries

### 3.1 `IM3` entry (`mgIMG_HEADER`, `0x40` bytes)

| Offset | Size | Type | Field | Description |
|---|---|---|---|---|
| `0x00` | `32` | `char[0x20]` | `name` | NUL-terminated entry name (1..19 chars; 6,207 entries have leftover bytes after NUL). |
| `0x20` | `4` | `u32` | `unk_20` | Always `0x40` (entry header size). Never read by the engine. |
| `0x24` | `4` | `s32` | `offset` | Byte offset from start of `.img` file (strictly ascending, contiguous, 16-byte aligned). |
| `0x28` | `4` | `s32` | `swizzled` | `1` when 8-bit pixels use `PSMCT32` swizzled order (16,171 entries); `0` when linear (123 entries). |
| `0x2C` | `4` | `s32` | `block` | Texture-block group offset (`0..7`) relative to the archive's base block. |
| `0x30` | `2` | `s16` | `no_image` | `1` (228 entries): registers texture and reserves GS VRAM, but stores no pixel or palette bytes. |
| `0x32` | `2` | `s16` | `unk_32` | `0` (3 junk values in `d01e01.img`). |
| `0x34` | `4` | `s32` | `size` | Byte length of TIM2 or script. Read **only** for `#` script entries (11 picture entries are short by 276 or 302 bytes). |
| `0x38` | `8` | `u64` | `clamp` | GS `CLAMP` register: `0x5` = clamp U/V (13,297 entries), `0x0` = repeat U/V (1,813 entries), junk in 6 entries. |

- **Texture animation scripts (`#texanime`)**: Any entry whose name starts with `#` is a Shift-JIS text script parsed via `LoadCFGFile(img + offset, size, ...)`, e.g.:
  ```
  SRC_TEX "g0301_17",0,0,64,64;
  DEST_TEX "g0301_15",0,0;
  ```
  Renaming or resizing a texture referenced by `#texanime` breaks its animation.

### 3.2 `IM2` and `IMG` entry (`mgIMG1_HEADER`, `0x30` bytes)

| Offset | Size | Type | Field | Description |
|---|---|---|---|---|
| `0x00` | `32` | `char[0x20]` | `name` | NUL-terminated entry name (junk bytes after NUL are common). |
| `0x20` | `4` | `s32` | `offset` | Byte offset of the TIM2 picture from the start of the `.img` file. |
| `0x24` | `12` | `u8[12]` | `pad` | Unused padding (19 of 111 `IM2` entries contain leftover junk). |

| Archive Version | Default `swizzled` | Block & Clamp Fields | Special Corpus Notes |
|---|---|---|---|
| `IM3` | Per-entry (`entry.swizzled`) | Explicit `block`, `clamp`, `no_image`, `size` | Standard format (5,155 files). |
| `IM2` | Implicitly `1` (`not_im2 == 0`) | None (loads into 1 block, spills if full) | 58 files (111 entries). |
| `IMG` | Implicitly `0` | None (loads into 1 block, spills if full) | 3 files. `item/atgrow.chr.d/atgrow.img` and `item/atluck.chr.d/atluck.img` have header entry count `0` and load 0 pictures. |

### 3.3 Data layout

- Across all 15,228 TIM2 pictures on the disc, consecutive entries have **0 bytes of slack**, never share an offset, and always align to 16 bytes.
- Normal TIM2 entry byte size: `16 + header_size + image_size + clut_size`.
- `no_image` TIM2 entry byte size: `16 + header_size` (pixel and palette sections omitted).

## 4. TIM2 picture

Each picture entry embeds a single-picture TIM2 container (`TM2_head` + `TM2_PICTURE`). Offsets are relative to the TIM2 start:

| Offset | Size | Type | Field | Corpus Values | Read by Engine? |
|---|---|---|---|---|---|
| `0x00` | `4` | `char[4]` | `magic` | `"TIM2"` | No |
| `0x04` | `1` | `u8` | `version` | `3` (12,284) or `4` (2,716) | No |
| `0x05` | `1` | `u8` | `format_id` | `0` (16-byte alignment) | No |
| `0x06` | `2` | `u16` | `pictures` | `1` | No (always reads picture 0) |
| `0x08` | `8` | `u8[8]` | `reserved` | All `0` | No |
| `0x10` | `4` | `u32` | `total_size` | See exporter quirks table below | No |
| `0x14` | `4` | `u32` | `clut_size` | `1024` (8-bit), `64` (4-bit), or `0` | No |
| `0x18` | `4` | `u32` | `image_size` | Exact byte sum of all mip levels | **Yes** (locates CLUT after pixels) |
| `0x1C` | `2` | `u16` | `header_size` | `0x30` (1 mip level) or `0x50` (with mip table) | **Yes** (locates start of pixels) |
| `0x1E` | `2` | `u16` | `clut_colors` | `256`, `16`, or `0` | No (assumes `256` for 8-bit, `16` for 4-bit) |
| `0x20` | `1` | `u8` | `pict_format` | `5` (11,761) or `0` | No |
| `0x21` | `1` | `u8` | `mip_levels` | `1`, or `3` (in 31 `PSMT8` pictures) | **Yes** |
| `0x22` | `1` | `u8` | `clut_type` | `0x03` (`PSMCT32`, `CSM1`) or `0` | No |
| `0x23` | `1` | `u8` | `image_type` | `1`=16-bit, `2`=24-bit, `3`=32-bit, `4`=4-bit, `5`=8-bit | **Yes** |
| `0x24` | `2` | `u16` | `width` | `32..1024` | **Yes** |
| `0x26` | `2` | `u16` | `height` | `32..1024` | **Yes** |
| `0x28` | `8` | `u64` | `GsTex0` | `0` in v3; v4 sets `PSM`, `TW`, `TH` (`TBW = 0`) | No (engine builds its own `TEX0`) |
| `0x30` | `8` | `u64` | `GsTex1` | v3 stores `K` in high word; v4 stores `0x260` | **Yes** (reads `L` and `K` when `mip_levels > 1`) |
| `0x38` | `4` | `u32` | `GsTexaFbaPabe` | `0` | No |
| `0x3C` | `4` | `u32` | `GsTexClut` | `0` | No |
| `0x40` | `16` | `u64[2]` | `MipTBP1`, `MipTBP2` | `0` (present when `header_size > 0x30`) | No |
| `0x50` | `4*n` | `u32[n]` | `mip_sizes` | Byte size of each mip level (trailing bytes may have junk) | **Yes** |

- **Data order**: Pixel data starts at `0x10 + header_size` (largest mip level to smallest), followed immediately by the CLUT palette.
- **Exporter quirks in `total_size` (`0x10`)**: In the `4 * total_pixels` variants below, the exporter multiplied all pixels by 4 bytes as if `PSMCT32`:

| `version` (`0x04`) | `pict_format` (`0x20`) | `total_size` (`0x10`) Formula | Corpus Count |
|---|---|---|---|
| `3` | `5` | `header_size + 4 * total_pixels` | `11,761` |
| `4` | `0` | Exact byte size (`header_size + image_size + clut_size`) | `2,716` |
| `3` | `0` | Exact byte size | `314` |
| `3` | `0` | `header_size + 4 * total_pixels` | `209` |

## 5. Pixel formats

`EnterTexture` maps `image_type` (`0x23`) to bits per pixel and a PS2 GS pixel storage mode (`PSM`):

| `image_type` | BPP | GS `PSM` | Pixel Storage in File |
|---|---|---|---|
| `5` | `8` | `PSMT8` | 1 byte per palette index. Stored in **swizzled `PSMCT32` order** when `swizzled == 1` (section 6.1), or linear raster order when `swizzled == 0`. |
| `4` | `4` | `PSMT4` | Linear raster order, 2 pixels per byte, **low nibble = left pixel**. Never swizzled (engine ignores `swizzled` for 4-bit, even though 275 of 278 set `swizzled = 1`). |
| `3` | `32` | `PSMCT32` | Linear raster order, 4 bytes per pixel (`R, G, B, A`). |
| `2` | `24` | `PSMCT24` | Linear raster order, 3 bytes per pixel (`R, G, B`). |
| `1` | `16` | `PSMCT16` | Linear raster order, `u16` `A1B5G5R5` (Red in bits `0..4`). Unused on the disc. |

**GS `TEX0` construction** (`SCE_GS_SET_TEX0(0, tbw, psm, tw, th, 1, 0, 0, 0, 0, 0, 1)`):

| Field | Value / Formula | Description |
|---|---|---|
| `TBW` | `ceil(width / 64)` | Texture buffer width in 64-pixel units. |
| `TW`, `TH` | `ceil(log2(width))`, `ceil(log2(height))` | Power-of-two dimensions (non-power-of-two sizes occur in 194 widths and 915 heights). |
| `TCC`, `TFX` | `1`, `0` | Include texture alpha (`TCC = 1`); modulate texture with vertex colour (`TFX = 0`). |
| `CPSM`, `CSM`, `CLD` | `PSMCT32`, `CSM1`, `1` | Set on indexed (`PSMT8` and `PSMT4`) textures. |
| `TBP`, `CBP` | Runtime VRAM block address | Assigned when uploading to GS VRAM. |

## 6. Swizzling, palettes, and alpha

### 6.1 The 8-bit swizzle (`swizzled = 1`)

A swizzled `w × h` `PSMT8` mip level is stored on disk as a **`(w / 2) × (h / 2)` `PSMCT32` image** (row stride `2 * w` bytes):
- **Large or mipmapped textures (`w * h > 65,536` or `mip_levels > 1`)**: `ReloadTexture` (`src/mg_texture.cpp:1252-1258`) DMA-uploads the data as `PSMCT32` at `width / 2`, `height / 2`, and `max(1, TBW / 2)`. When the GS samples those pages as `PSMT8`, the pixels read back in exact `w × h` order.
- **Small single-level textures (`w * h <= 65,536`)**: `EnterTexture` unswizzles the pixels **in place** to linear `PSMT8` order at load time via `Conv32To8` -> `PageConv32to8` -> `BlockConv32to8`, clears `swizzled = 0`, and enables Z-buffer VRAM caching (`CheckCopyToZBufVram`).

The swizzle operates inside 16×16 pixel (256-byte) blocks (`block_table8_1266` and `block_table32_1267` have identical block layouts within a GS page). For an 8-bit pixel at `(x, y)`:

```python
# Table lookup form (inverse of BlockConv32to8 using lut_1246):
bx, ix = divmod(x, 16)
by, iy = divmod(y, 16)
j      = iy * 16 + ix
i      = BLOCK8_INV[j]  # inverse of: j = (i // 64) * 64 + lut_1246[((i // 64) & 1) * 64 + (i % 64)]
offset = (by * 8 + i // 32) * (2 * w) + bx * 32 + i % 32

# Equivalent closed-form bitwise formula:
block  = (y & ~15) * w + (x & ~15) * 2
swap   = (((y + 2) >> 2) & 1) * 4
posY   = (((y & ~3) >> 1) + (y & 1)) & 7
column = posY * w * 2 + ((x + swap) & 7) * 4
byte   = ((y >> 1) & 1) + ((x >> 2) & 2)
offset = block + column + byte
```

**Dimension constraints**:
- `w` must be a multiple of 16.
- Non-multiple-of-16 heights occur on 128×876, 128×680, and 128×748 textures (multiples of 4; no pixels lost) and on four **256×255** textures (`map/f/f04/f04.ipk.d/f0401.img`), where 128 pixels in the bottom two rows are unstored and `Conv32To8` reads 256 bytes past the pixels into the palette.
- In mipmapped swizzled textures, each mip level is swizzled independently at its own dimensions.

### 6.2 Palettes (CLUT)

All palettes store 32-bit `R8G8B8A8` entries (`CPSM = PSMCT32`) immediately after the last mip level:

| Format | Entries (Bytes) | Upload Dimensions | Entry Order in File |
|---|---|---|---|
| `PSMT8` | `256` (`1,024` B) | 16×16 `PSMCT32` (`0x40` qwords) | **`CSM1` swizzled**: entries `8..15` and `16..23` swap in every 32-entry group (`file_idx = (i & 0xE7) \| ((i & 8) << 1) \| ((i & 16) >> 1)`). Applied to all 8-bit textures regardless of TIM2 `clut_type` bit 7. |
| `PSMT4` | `16` (`64` B) | 8×2 `PSMCT32` | Linear order (`0..15`). |

### 6.3 Alpha (`0x80 = 1.0`)

The PS2 GS treats alpha `0x80` (`128`) as full opacity (`output = (texture * vertex_colour) / 0x80`):

| Texture Category | Alpha Range in Corpus |
|---|---|
| Indexed (`PSMT8` / `PSMT4`) | Max alpha is `128` in 11,964 pictures, `127` in 2,192, lower in the rest, and `> 0x80` (`0..255`) in only 1 texture (`map/s/s51/s51.ipk.d/s51.img` -> `s510121`). |
| `PSMCT32` | `0..0x80`, except 105 textures with alpha `0` on every pixel (77 have non-black RGB; 6 are `*_wrk` render targets). |
| `PSMCT24` | No stored alpha channel. |

## 7. How the engine loads and finds textures

**`EnterIMGFile(img, block, stack, info)` load sequence**:
1. **Sort (`IM3` only)**: Sorts the entry table in place by `entry.block` (unstable exchange sort; keeps `#` script entries fixed and treats negative `block` as `0`).
2. **Register entries**:
   - `#` entries run `LoadCFGFile` to register texture animations for the current block.
   - Picture entries run `EnterTexture(block + spill + entry.block, name, tim2, swizzled, no_image)`, copy `entry.clamp` to `texture->clamp`, and update `entry.swizzled` (`0` if `Conv32To8` unswizzled in place).
3. **VRAM spill accounting**: Each texture rounds up to whole 32-block GS pages plus 4 GS blocks per CLUT. If `GetRemainVRAM < 0`, the engine prints `"texture over"` and increments `spill`, shifting that texture and subsequent entries into the next texture block.

**Name registration and hash lookup**:
- `EnterTexture` concatenates `name + name_suffix` into a 32-byte (`0x20`) buffer (**max 31 characters**). Duplicate names log a warning.
- Registered textures insert into a 101-bucket global hash table (`h = ((h << 8) + (signed char)c) % 101`).
- `GetTexture(name, block)` appends the active `name_suffix` (`SearchTextureName`) and searches the hash table with `strcmp` (`block < 0` searches all blocks).
- 3D materials (`src/mg_visual.cpp:187`), `#texanime` scripts (`src/mg_tanime.cpp:650, 676`), and C++ code (`GetTexture("water", -1)`) all resolve textures by name.

## 8. Writing rules (what a modded file must satisfy)

| Rule | Requirement |
|---|---|
| Texture names | Keep original names (`<= 31` bytes including any runtime suffix). |
| Dimensions and format | Preserve original width, height, and pixel format where possible to avoid breaking `#texanime` rectangles or overflowing GS VRAM blocks. |
| Pixel alignment | Align every mip level's start offset and byte size to 16 bytes (DMA transfers truncate to whole 16-byte quadwords: `qwc = (bpp * w * h) / 128`). |
| Palettes | Use 16 (`PSMT4`, 64 B) or 256 (`PSMT8`, 1,024 B) `RGBA32` entries. Store 8-bit palettes in `CSM1` swizzled order. |
| 8-bit swizzle | If `swizzled = 1` on `PSMT8`, width must be a multiple of 16 (`<= 128` or a multiple of 128), and heights `> 64` must be a multiple of 64 (`Conv32To8` steps in 64-row pages). Alternatively, set `swizzled = 0` and write linear `PSMT8` pixels. |
| Mipmaps | Maximum 4 levels (`MG_TEXTURE_LEVEL_MAX`), halving width and height each level, with sizes listed at TIM2 offset `0x50` (`header_size = 0x50`). |
| Archive offsets | Keep all entry offsets contiguous and 16-byte aligned, and update `IM3` `size` fields to match. On-disk `block` sorting is optional because `EnterIMGFile` sorts in memory. |

## 9. Open problems

- **Alpha edge cases**: Whether over-bright alpha (`0..255`) in `s510121` was intentional, and how the 105 all-zero-alpha `PSMCT32` textures configure blending at draw time, are not traced.
- **256×255 textures (`f0401.img`)**: `Conv32To8` reads 256 bytes past the pixel array into the palette, displaying palette bytes in the bottom two rows.
- **Exporter junk**: 11 `IM3` entries have `size` short by 276 or 302 bytes; `e207a01.img` and `d01e01.img` have junk in `clamp` and `unk_32`; `c14a01.img` has `0x40` and `0x21` in header words `0x08` and `0x0C`; `pict_format = 5` appears in 11,761 TIM2 headers. None affect runtime loading.
- **GS VRAM block spillover**: Increasing texture dimensions can trigger `"texture over"` and spill into adjacent texture blocks. See [memory budgets](memory.md) for per-scene block limits.
