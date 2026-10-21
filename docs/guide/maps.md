# Maps and Blender

`dc2tk setup` exports the game's 3D maps into `extracted/maps/<map-code>/` as `.gltf` scenes for Blender, for reference. To change a map, export it into a mod.

## Quick steps

1. Export the map into your mod folder:
   ```sh
   dc2tk map-export m01 -o mods/my-mod/maps/m01
   ```
2. Open `mods/my-mod/maps/m01/m01.gltf` in Blender with the [dc2tk add-on](#installing-the-blender-add-on), edit placements, trigger points, collision, meshes, or textures, and press **Export Map to Mod**. The add-on writes the scene back and adds the map's line to `mods/my-mod/mod.json`:
   ```json
   {
     "replace": {
       "map:m01": "maps/m01/m01.gltf"
     }
   }
   ```
   (Without the add-on, export the scene as **glTF Separate** with custom properties and +Y up, and add the line yourself.)
3. Build the ISO with `dc2tk build`. It rewrites only what changed: the `.map` file, edited or new models, and edited or new textures. An unedited scene changes nothing.

To see a change in the game without building an ISO, press **Play in Game** (F5) in the add-on instead of step 3. It exports the map, builds it into the native port's override folder, and shows it in the port's debug build with the player at the 3D cursor, starting the game first if it is not running. Press F5 again after each edit; **Cursor to Player** takes the 3D cursor back to where the player stands in the game. This needs the debug build (`python3 -I tools/port.py --movies --debug`) and the retail ISO set in the add-on's preferences.

`dc2tk map-export --list` lists every map with its title. Dungeon floor libraries such as `d01f01` are not in `extracted/maps`; export them the same way (see [Dungeons](dungeons.md)).

## Installing the Blender add-on

1. Download `dc2tk-<version>-blender.zip` from [GitHub Releases](https://github.com/fakegenie/DC2-Toolkit/releases).
2. In Blender (4.2+), open **Edit > Preferences > Add-ons**, click the menu at the top right, choose **Install from Disk**, and select the zip file.
3. In the add-on's preferences (**Dark Chronicle Maps (dc2tk)**), set the **dc2tk repository** folder to your unzipped `dc2tk` release.
4. Press `N` in the 3D Viewport and open the **Dark Chronicle** tab. Pick a map from the list and press **Import**.

The add-on also checks a map against the game's limits (**Validate Map**), shows what a build would change (**Preview Changes**), and builds the ISO. See [its README](../../blender-plugin/README.md) for everything it does.

## The glTF scene

The scene root is `map:<name>`, with four groups:

| Group | Contents |
| --- | --- |
| **`placements`** | The map's placed parts (`place:NAME`), each holding its pieces: visible models, and collision meshes (`kind: collision` for floors and walls, `kind: camera` for camera collision). |
| **`points`** | Trigger points (`point:NAME`): doors, chests, save points, lights, sounds, spawn positions, and more. A point's `extras.c2` holds its kind and data (a door's `program` and `target`). |
| **`cameras`** | Fixed camera positions. |
| **`sky`** | The sky, for display only; it is never built. |

Placements keep their slot numbers because scripts address them by slot: deleted placements stay in the map, hidden, and new ones are added at the end.

## A map of a mod's own

### A new map

Add a new map under `"add"` in `mod.json`, using an existing map as a `"like"` template:

```json
{
  "add": {
    "map:fishroom": {
      "file": "maps/fishroom/fishroom.gltf",
      "like": "m07",
      "title": "The Fish Room",
      "script": "fishroom.c",
      "programs": {
        "door_out": "LEAVE_FISH_ROOM"
      }
    }
  }
}
```

| Option | Meaning |
| --- | --- |
| `"like"` | The disc map to copy as a base. |
| `"title"` | The map's title in the map table (defaults to the `"like"` map's). |
| `"script"` | The C file in your mod that becomes the map's script. |
| `"programs"` | Script programs for the scene's event points, by point name: a number, or a `NEW_PROGRAM` name. |
| `"compact"` | `true` drops deleted placements instead of keeping them hidden. |

A map name is lowercase letters, digits, and `_`, at most 12 characters, and not a map on the disc. Scripts see the new map's number as `MAP_<NAME>` (`MAP_FISHROOM`). In the Blender add-on, **New Map from This One** writes this entry and a starter script for you.

### A new map's own script

The `"script"` file is a complete map script, compiled once per language. When the map loads, the engine runs its `map_start` program, which must place the player (`place_chara`), position the camera, and fade in the screen, like the game's town scripts. See [The Spinning Cult](../scripting/examples/spinning-cult/README.md) (`fishroom.c`) for a working example.

### Doors between maps

A door is an `event` point of kind `door` in a map's scene, plus a program in that map's script:

1. Add the door point in Blender (**Add > Dark Chronicle > Door**), or use **Door to Another Map**, which also writes the program and the `"programs"` entry.
2. Name the door's program on the map's line in `mod.json`, so the scene never holds a generated number:
   ```json
   "replace": {
     "map:m01": { "file": "maps/m01/m01.gltf", "programs": { "door_fish": "ENTER_FISH_ROOM" } }
   }
   ```
3. Write that program in a script patch for the map. It jumps to the new map:
   ```c
   #pragma target map m01
   #include <event.h>
   #include "cult.h"

   void enter_fish_room(void)
   {
       jump_to_map(-1, "fishroom", map_start);
       WAIT();
   }

   PROGRAM(ENTER_FISH_ROOM, enter_fish_room);
   ```

Declare `NEW_PROGRAM(ENTER_FISH_ROOM)` in a header the mod's scripts share (here `cult.h`). The way back works the same: a door in the new map whose program jumps back to the town. See the spinning-cult mod's `m01.c` and `fishroom.c`.

## Limits

- At most 320 placements in a map (400 in dungeons), and template names of at most 31 characters.
- Keep the names of placements, pieces, and points that the map's scripts use.
- `dc2tk build` checks every map memory budget (see [Memory Limits](memory.md)).

## Scene reference

For editing a scene as raw JSON, or writing tools for it. Every exported node carries `extras.c2`, whose `"role"` says what it is:

| Node | Name | `extras.c2` |
| --- | --- | --- |
| Scene root | `map:NAME` | `{"role": "map", "map": {...}}`: the `.map` file's settings, images, packs, light sets, and cameras |
| Group | `placements`, `points`, `cameras` | `{"role": "group"}` |
| Placement | `place:NAME` | `{"role": "placement", "template", "show", "group", "far_clip", "slot"}`; its transform is the placement's |
| Piece | `piece:NAME` | `{"role": "piece", "model", "kind", "show", "time", "col_type", "materials"}`; `kind` is `model`, `collision`, `camera`, or `chara`; its children are the model's nodes |
| Model node | the node's full name | `{"role": "mds", "mds_index": n}`; meshes are shared by every piece that places the model |
| Trigger point | `point:NAME` | `{"role": "point", "kind", "enable", "flag", "data": {...}}`; under its placement when part of a template, else under `points` |
| Fixed camera | `camera:INDEX` | `{"role": "camera", "index"}` at the camera's position |
| Sky | `sky`, `sky:day`, ... | `{"role": "display"}`: never built |

- **Units and axes**: 1 glTF unit is 1 game unit (a dungeon cell is 320 across), Y up, as in the game. Rotations are written back as the game's Euler angles; nodes that did not move keep their original numbers.
- **Names and order**: Blender renames duplicates (`.001`) and sorts children by name, so every node keeps its exact name in `extras.c2.name` and its position in `extras.c2.order`. New nodes take their Blender name without the role prefix and `.NNN` suffix, and go after the existing ones.
- **Templates**: Each placement holds its own copy of its template's pieces and points. A change made the same way in every placement of a template changes the template; a change in only some placements makes a new template for them.
- **Collision**: A collision piece's material names hold its attributes (`col_ground0_foot2_area6_mask12`). See [DC2 Internals: Maps](../internals/maps.md) section 4.3 for what they mean.
- **Point data** (`data`, by the point's `kind`):
  - `event`: `kind` (`door`, `t_box`, ...), `event_no`, `program`, `args[3]`, `target`, `flags[3]`
  - `invent`: `id`, `min[3]`, `max[3]`, `unknown_int`, `unknown_float`, `angle`
  - `plight`: `power`, `color[3]` (`0`–`511`), `mode`, `args[4]`, `flicker`, `depth`, `period`
  - `sound`: `number`, `near`, `far`, `unknown_int`, `shape`, `start[3]`, `end[3]`
  - `anime`: `placement`, `piece`, `frame`, `drives`, `mode`, `start[3]`, `step[3]`, `end[3]`, and `uniform`, `relative` when present
  - `fire`, `flare`: `color[3]`, `draw`, `extra[2]`; `effect`: `effect`; `pos`: none
- **Other fields**: a point's `flag` is `[a, b, start_hour, end_hour]`; a piece's `time` is `[start, end]`; a placement's `far_clip` is `[distance, fade]` or `null`; a piece's `col_type` is an integer, `[type, param]`, or `null`.

See [DC2 Internals: Maps](../internals/maps.md) for `.map`, `.pcp`, `.mpk`, and `.ipk` binary specifications.
