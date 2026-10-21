# Dungeons

Random dungeon floors in *Dark Chronicle* are built at runtime from room layouts and 3D parts. Mods change them through five data tables and the floors' maps.

## Quick steps

1. Copy a dungeon data table into your mod folder:
   ```sh
   dc2tk mod asset dc2.iso my-mod data:floors
   ```
   This writes `mods/my-mod/data/floors.json` and adds `"floors": "data/floors.json"` under `"data"` in `mod.json`.
2. Open `mods/my-mod/data/floors.json` and keep only the floors you change.
3. Build the ISO with `dc2tk build`.

## Dungeon data tables

Dungeons are numbered from `0` (the Underground Channel) to `7`.

| Table ID | Controls |
| --- | --- |
| `data:floors` | Each floor's monster spawns (`monsters`: up to 32 `{"monster", "talk"}` entries) |
| `data:medals` | Each floor's medal goals: `fast_time` (in 1/60 s), `fishing_record`, `seal`, `spheda`, `spheda_prizes`, and the practice medal |
| `data:chests` | Treasure chest contents: item `groups` (items by `rank`), and the groups each floor draws from |
| `data:layouts` | Room layouts: each layout's `grid_size` and `rooms`, written as rows of part names |
| `data:parts` | The 277 dungeon parts (name, kind, link) and each floor's minimap tiles |

See [Game Data](data.md) for how tables merge and what can be added.

## Example: Floor monsters and time medal

Change the monsters and the time medal of the Underground Channel's first floor:

```json
{
  "floors": [
    {
      "dungeon": 0,
      "floor": 1,
      "monsters": [
        { "monster": 0, "talk": 0 },
        { "monster": 0, "talk": 1 },
        { "monster": 168, "talk": 2 }
      ]
    }
  ]
}
```

```json
{
  "floors": [
    { "dungeon": 0, "floor": 1, "fast_time": 7200 }
  ]
}
```

The first goes in `data/floors.json` (`data:floors`), the second in `data/medals.json` (`data:medals`, a 2-minute goal). Monster `0` is the Sewer Rat and `168` the Golem; `talk` is the spawn's talk parameter (`-1` for a monster you cannot talk to).

## Example: Room layouts

In `data:layouts`, each room's `cells` is a list of rows of part names (`.` is an empty cell, and `:N` adds a cell attribute). Give a changed room's `cells` in full, or change only its `rate`:

```json
{
  "layouts": [
    { "layout": "d01f01", "rooms": [{ "room": 5, "rate": 0 }] }
  ]
}
```

A room numbered right after a layout's last room adds a new room (at most 64 per layout). Rooms cannot be deleted: set `"rate": 0` to keep one out of random floors.

## Edit 3D dungeon parts

A dungeon's parts are templates in its floor library map (`d01f01` for the Underground Channel). Export it into your mod:

```sh
dc2tk map-export d01f01 -o mods/my-mod/maps/d01f01
```

The scene lays out each part once, in rows. Edit their meshes, collision, and trigger points in Blender as on any [map](maps.md), and add `"map:d01f01": "maps/d01f01/d01f01.gltf"` under `"replace"`. Keep each part inside its 320 × 320 cell (400 × 400 in `d05`, `d06`, and `mstage4` to `mstage6`), with its open sides where its `link` says, because the floor generator joins parts edge to edge.

None of these dungeon edits has been tested in the game yet.

See [DC2 Internals: Dungeon Floors](../internals/dungeons.md) for how the game builds a floor.
