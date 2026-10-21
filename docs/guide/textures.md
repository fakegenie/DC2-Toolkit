# Textures (PNG)

`dc2tk setup` exports all game textures as `.png` files in `extracted/assets/textures/`.

## Quick steps

1. Copy a texture into your mod folder by its asset ID (listed in `extracted/assets/manifest.json`):
   ```sh
   dc2tk mod asset dc2.iso my-mod texture:e254a1
   ```
2. Edit `mods/my-mod/textures/e254a1.png` in an image editor.
3. Verify the entry in `mods/my-mod/mod.json`:
   ```json
   {
     "replace": {
       "texture:e254a1": "textures/e254a1.png"
     }
   }
   ```
4. Build the ISO with `dc2tk build`. `dc2tk` converts the PNG to TIM2 format, regenerates mipmaps, and updates all copies on the disc.

## Export an entire `.img` archive

To export all textures from one `.img` archive as a folder of PNGs:

```sh
dc2tk mod asset dc2.iso my-mod chara/c02a.chr.d/c02a.img --as png
```

This writes one PNG per texture into `mods/my-mod/raw/chara/c02a.chr.d/c02a.img.png/` and adds the folder's line to the mod's `"files"`. Delete the PNGs you do not change, edit the rest, and run `dc2tk build`.

## Add a new texture archive (`.img`)

Create a new `.img` archive from a folder of PNGs with a `"mod:NAME"` line in `"files"`. Scripts load it with `load_image` (as `MOD_DIR "calc_ui.img"`) and show its textures as sprites by name:

```json
{
  "files": {
    "mod:calc_ui.img": {
      "file": "ui",
      "like": "texture:logo_d00"
    }
  }
}
```

| Field | Description |
| --- | --- |
| `"file": "ui"` | Packs each `.png` in `mods/<name>/ui/` as a named texture (for example, `ui/calc_body.png` becomes `"calc_body"`). |
| `"like": "texture:logo_d00"` | Formats textures as 8-bit 256-color paletted images with transparency for 2D UI sprites. |

```c
load_image(MEMORY_STACK_EVENT, "mods/calculator/calc_ui.img", UI_IMAGE, &block);
set_sprite_texture(SPRITE_CURSOR, "calc_cursor_wide", block);
```

See [`calculator.c`](../scripting/examples/calculator/calculator.c) for a working example.

## Rules and limits

| Texture Type | Requirement |
| --- | --- |
| **All textures** | Width and height from 1 to 1024 pixels. Keep the original name of every texture you replace. |
| **3D model and map textures** | Power-of-two width and height (`64x64`, `128x128`, `256x256`). |
| **Swizzled 8-bit textures** | Width must be a multiple of 16. |
| **Animated map textures (`#texanime`)** | Keep the original pixel dimensions. |
| **New texture names** | 1 to 31 Latin-1 characters, unique ignoring case. |

A larger texture can push later textures of the same archive into the next VRAM block; `dc2tk build` checks VRAM budgets (see [Memory Limits](memory.md)).

See [DC2 Internals: Textures](../internals/textures.md) for `.img` and TIM2 binary specifications.
