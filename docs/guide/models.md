# 3D Models (glTF)

`dc2tk setup` exports character, monster, weapon, and item 3D models (including skeletons, textures, and animations) into `extracted/assets/models/` as `.gltf` files.

## Quick steps

1. Copy a model into your mod folder:
   ```sh
   dc2tk mod asset dc2.iso my-mod model:e33a
   ```
   This writes `mods/my-mod/models/e33a/` (`e33a.gltf` and `e33a.bin`) and adds `"model:e33a": "models/e33a"` under `"replace"` in `mod.json`.
2. Set Blender's frame rate to **30 FPS** (**Output Properties > Format > Frame Rate**), then import `mods/my-mod/models/e33a/e33a.gltf` (**File > Import > glTF 2.0**).
3. Edit the meshes, textures, or animations, and export back over `e33a.gltf` with **Format** set to **glTF Separate (`.gltf` + `.bin` + textures)**.
4. Build the ISO with `dc2tk build`. Only the meshes and animations you changed are rebuilt.

A pattern copies every model it matches: `'model:c02fuku*'` copies Monica's ten outfit models (`c02fuku1` to `c02fuku5`, and `c02fuku1t` to `c02fuku5t`). To export a whole character pack instead, run `dc2tk mod asset dc2.iso my-mod chara/c02a.chr --as gltf`, which writes `mods/my-mod/raw/chara/c02a.chr.gltf/` and adds `"chara/c02a.chr": "raw/chara/c02a.chr.gltf"` to the mod's `"files"`.

### Max and Monica

Max and Monica's outfits are separate packs (`mainchr/c01fuku1.chr` and similar). Their skeletons and animations are in base packs: one for towns and event scenes (`chara/c01_base.chr`, `chara/c02_base.chr`) and one per weapon model in dungeons (`mainchr/c01_base2.chr` to `c01_base4.chr`, `mainchr/c02_base0.chr` to `c02_base3.chr`). To change how they move everywhere, edit each base pack.

## Animations (`.mot` / Blender Actions)

Character animations import into Blender as **Actions**, one per motion:

| Action Edit | In-Game Result |
| --- | --- |
| Modify an existing Action | Updates that animation on the model. |
| Create a new Action | Adds a new motion (`KEY` in `info.cfg`) that scripts play by name with `set_chara_motion`. |
| Delete an Action | Removes its motion. Scripts that pick motions by number see the later ones shift down by one. |

Motions in sets other than `0` are Actions named `<set>:<name>` (such as `3:bow`).

## Add a new model pack (`.chr`)

Add a `"mod:NAME"` line to `"files"` in `mod.json` to build a new `.chr` model pack from a glTF file. Scripts load it with `load_chara` (or the `load_scene_chara` helper) from `MOD_DIR`, the mod's own folder on the disc:

```json
{
  "files": {
    "mod:gill.chr": {
      "file": "model/gill.gltf",
      "like": "model:p01_01a"
    }
  }
}
```

```c
load_chara(MEMORY_STACK_EVENT, MOD_DIR "gill.chr", "info.cfg", GILL);
```

| Template (`"like"`) | Use Case |
| --- | --- |
| `"model:p01_01a"` (a townsperson) | Animated character: the glTF armature becomes the skeleton, and every Action becomes a motion (see [The Spinning Cult](../scripting/examples/spinning-cult/README.md)). A skinned model needs at least one Action. |
| `"model:uki"` or `"model:takaraky"` | Static prop or item mesh (see [Calculator](../scripting/examples/calculator/calculator.c)). |

```c
load_chara(MEMORY_STACK_EVENT, "mods/gill/gill.chr", "info.cfg", slot);
set_chara_motion(slot, "spin");
```

## Rules and limits

| Property | Limit |
| --- | --- |
| **Skeleton** | Keep the original bones, names, and order: animations and cloth find them by name and number. |
| **Bone weights** | At most **4 bones** per vertex and **32 bones** per skinned mesh. |
| **Skinned mesh size** | At most **3,198 vertices** per skinned mesh, and at most 24 skinned meshes per model. |
| **Texture dimensions** | Power-of-two width and height (`64x64`, `128x128`, `256x256`). |
| **Party memory** | Hero outfits and weapons must fit inside fixed party memory stacks (see [Memory Limits](memory.md)). |

See [DC2 Internals: 3D Models](../internals/models.md) for `.mds`, `.wgt`, `.bbp`, and `.mot` binary specifications.
