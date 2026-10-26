# dc2tk_maps: a Blender add-on for DC2 maps

`dc2tk_maps` imports a map that `dc2tk map-export NAME` wrote as a glTF scene, shows
it as the game draws it, lets you edit its placements, trigger points,
collision, settings, meshes and textures and model new objects, checks it
against the game's limits, and exports it into a mod that `dc2tk build` turns
into a disc image. The scene's conventions (node names, `extras.c2`,
collision material names) are in [the glTF scene](../docs/guide/maps.md#scene-reference),
and the formats behind them in [maps](../docs/internals/maps.md); the add-on follows them and never renames what carries data.

Blender 4.2 or newer (tested with 4.2.23, 5.1.2 and 5.2.2); written to run on 3.6 too. Plain
Python, nothing to install besides the add-on.

Besides the sidebar it is in Blender's own menus: Add (Shift+A) > DC2
adds every kind of point, a door to another map, a template of any
map, a placement copy, a new model and a player to measure by; right-clicking
an object gives Drop to Ground, Copy Game Coordinates and the template
tools; Shift+Alt+D opens a pie of the common ones (door, light, spawn, chest,
drop to ground, duplicate, validate, play in game), and F5 plays the map in the
native port (Play in Game, below).

## Install

Either way, then set the repository folder in the add-on's preferences
(Edit, Preferences, Add-ons, "DC2 Maps (dc2tk)"). When the add-on is
linked from this repository, the repository is found by itself.

- **Install a release:** download `dc2tk-VERSION-blender.zip` from the repository's GitHub
  releases, then Edit, Preferences, Add-ons, Install from Disk (Blender 4.2+). Set the
  repository folder to the unzipped dc2tk release, or to a checkout.
- **Link it (follows the repository):** link `blender-plugin/dc2tk_maps` into
  Blender's add-ons folder and enable it.
  - Windows (Blender 4.2+: the extensions folder):
    `mklink /J "%APPDATA%\Blender Foundation\Blender\5.1\extensions\user_default\dc2tk_maps" "C:\path\to\DC2-Toolkit\blender-plugin\dc2tk_maps"`
  - Linux, macOS: `ln -s /path/to/DC2-Toolkit/blender-plugin/dc2tk_maps ~/.config/blender/5.1/extensions/user_default/dc2tk_maps`
    (`~/Library/Application Support/Blender/5.1/...` on macOS)
  - Blender 3.6-4.1: the same into `scripts/addons/` instead of
    `extensions/user_default/`.
- **Install a zip:** zip the `dc2tk_maps` folder (the zip holds `dc2tk_maps/...`),
  or `blender --command extension build --source-dir blender-plugin/dc2tk_maps`,
  then Edit, Preferences, Add-ons, Install from Disk.

Preferences:

| setting | default |
|---|---|
| dc2tk repository | the repository around the add-on, when linked |
| dc2tk executable | `build/release/dc2tk.exe` (`dc2tk` elsewhere), else `dc2tk.exe` (a release), `dc2tk.bat` or `./dc2tk` in the repository |
| map export arguments | `map-export {name} -o {out}` |
| exported maps | `extracted/maps` in the repository |
| mod folder, source tree, retail disc, output disc image | for Export and Build; the source tree defaults to `extracted/scripts`; the retail disc is the ISO or an untouched extracted disc folder |
| debug port, override folder, API port | for Play in Game: `build/port/dc2-debug`, `overrides` beside it, 7777 (the port's `DC2TK_DEBUG_PORT`) |

## Use

Everything is in the 3D viewport's sidebar (N), tab **DC2**, and
in File, Import.

**Import.** Pick the map from the list (it searches as you type: `palm`, `s17`)
or type its name, and press Import: the add-on runs
`dc2tk map-export s17 -o extracted/maps/s17` and imports the result. The list
is every map on the disc with its title ("m01  Palm Brinks"), read from
`dc2tk map-export --list` when the panel first shows; the refresh button reads
it again. File, Import, "DC2 map (.gltf)" imports a scene exported
before.

An import replaces the map already in the scene: the old map's objects, its
collections (once they hold nothing else) and the meshes, materials, images,
actions and world only it used are removed first, so maps don't pile up as
`.001` copies. Objects of your own outside the map stay. Untick "Unload other
maps" in the operator's options (F9, or the File, Import dialog) to keep the
old map beside the new one. An edit not yet exported into a mod goes with the
map it was made in.

The import keeps every custom property (`extras.c2`), merges no vertices and
keeps the node tree, then:

- moves collision (pieces of kind `collision`) and camera collision (`camera`)
  into the collections "NAME collision" and "NAME camera collision", hidden;
  the Collision and Camera col. buttons show them;
- colours collision by area (water areas 1 and 7 blue), foot sound, ground or
  ignore mask, with the materials' viewport colour only: the names
  (`col_ground0_foot2_area6_mask12`) are the data and stay as they are;
- shows trigger points as empties: event points as cubes of their size (doors
  15, ladders 25, chests 20 when the scale is 1), invent boxes as cubes and
  their data's min-max box drawn in the viewport, sounds as spheres of their
  near distance, animations (`anime`) as arrows with what they do drawn on
  what they move: a position's path from start to end, a rotation's axis and
  the arc it sweeps, and for the selected one a line to what it moves
  (Animations in the Map panel);
- makes point lights (`plight`) Blender point lights, with the glTF light's
  range as the light's custom distance;
- draws the materials as the game does (Material Preview and Rendered): the
  picture times the vertex colour, alpha from both (the fades painted into
  terrain), additive glows (waterfalls, light shafts) and subtractive ones,
  unlit meshes, and alpha tests that still blend (soft shadows). The draw
  flags come from the model's frame names (the material's `frame_flags`).
  Blender's own material stays in each node tree, unlinked; the export links
  it back while it writes, so what is written is the same;
- lights the scene as the map's active light set does (Rendered and F12):
  a world "c2 NAME" with its background colour behind everything and its
  ambient colour around, and its directional lights as suns ("c2 light NAME
  N", outside the map, never exported). A world of your own is kept;
- turns the 3D view to Material Preview with the scene's world and lights
  (the light set's, so the preview looks like the game rather than a studio)
  and frames where the map is played (its trigger points), leaving nothing
  selected;
- shows the pieces the game draws at the hour in the Map panel (noon to
  start): towns hold day and night versions of pieces (`PIECE_TIME`), and
  showing both at once covers the town. Pieces and placements the game hides
  at the start (show 0: pieces scripts show later, and placements deleted in
  an earlier edit, which keep their slot) are hidden too, also when you untick
  Shown. Untick the clock for every piece.
  Hidden collision is left out of renders too.

**What the scene shows besides the map.** The export carries the sky the
engine draws about the camera (the map's `.sky`, else `map/def.sky`): its
day, evening, night and morning skies, of which the add-on shows the one the
hour uses, never selectable and never exported. And what the map's `.cfg`
adds, drawn in the view (Water, culling in the Map panel): water surfaces,
occlusion planes, the areas whose parts are not drawn from some camera
spots, extra trigger points, and where villagers stand and face. Neither is
edited from the scene.

**Find your way.** The Map Outliner lists the map's placements, points
(of one kind, or all), pieces or fixed cameras, found by part of a name,
template, kind or model; a dimmed row is hidden or disabled, a page icon
marks what the map's scripts name. Picking a row selects it, Frame Selected
shows it.

**Edit.** Select something; the Selected panel shows what it is and its
fields, which write straight into `extras.c2`:

- placement: name, template, group, shown, far clip (or none), its slot;
- piece: name, model, shown, hours, collision type;
- point: name, kind, enabled, hours, flags, and the fields of its kind (an
  event's kind with a menu of door, event, t_box..., its event number,
  program, arguments, target and flags; a light's power, colour, mode and
  flicker; a sound's number, near and far; an invent box's id and corners...).
  A point light's colour also sets the Blender light's (r, g, b / 255). An
  animation shows the placement, piece and model node it moves (and whether
  the scene has them), what it drives (position, rotation in degrees, scale,
  colour, alpha) and how (loop, stop at the end, repeat, ping-pong, random,
  look at the player, a clock's hands, by the hour), with the start, step and
  end values that mode reads, labelled for it;
- "All values" lists every value of `extras.c2`, each with an edit button,
  for anything without a field of its own (cameras, piece materials).

With several objects of one role selected, "Edit all N selected" makes a
field's edit change all of them, as Alt does for Blender's own fields (a
point's data fields only those of its kind).

A placement's box (and a piece's or point's in it) says what the build will
do with its template: change it, when every placement of it is alike, or
give the placements that differ templates of their own. Apply to All gives
every placement of the template this one's pieces and points, Make Unique
gives this one a template of its own on purpose, and the arrow selects them
all. When the map's scripts name the object (Read Map Scripts), the box
lists the calls, file and line: renaming or deleting it breaks them, and
Validate says so.

Name fields update both the Blender object name and `extras.c2.name`, which
the build uses. Duplicated placements receive a new game name too. Names
are unique within their game scope; Blender may add a suffix to their display names.
The list editor accepts JSON, such as `[1, 2, 3]` or `["t01.img", "other.img"]`.

Map Settings shows the map root's settings (`extras.c2.map`: light sets,
fog, background colours, time, sky...) the same way.

Move, turn and scale with Blender's tools as usual. Clicking a mesh of the map
selects its placement (Click selects, in the Map panel: Placement, Piece or
Object), since that is what moves a building: a model's meshes are shared by
every placement of it, as in the game, so moving a mesh inside its model (Click
selects: Object) moves it in every placement of that model once built, though
Blender shows it moved in one. Points, cameras and placements are selected as
they are. Edit meshes as usual too: a mesh is shared by every piece that places
the same model, as in the game. Building rebuilds only the models whose meshes you changed (Blender's
own rewriting of untouched meshes is allowed for), and a rebuilt mesh keeps
the vertex alpha fades Blender leaves out.

**Lay out.** Drop to Ground moves the selected placements, points and objects
down onto the map's floor collision (or its drawn meshes), as the player
would stand there, optionally turned to the slope. Town snap sets the grid
to 10 units and turns to 15 degrees (the Georama step), Cell snap to dungeon
cells (320) and quarter turns. Copy Game Coordinates puts where the active
object or the 3D cursor is on the clipboard in game axes (y up), as `x, y,
z`, `place_chara(CHARA_PLAYER, x, y, z, yaw);`, `set_camera_pos(...)`,
`set_camera_target(...)` or `#define NAME_X ...` lines, the yaw as the game
turns things. Player for Scale stands Max (the asset tree's
`models/chara/c01_base`, else a cylinder of the player's radius, 15) at the
cursor, outside the map.

**Lighting.** The Lighting panel edits the map's light sets in place:
background colours, ambient, fog (on, near, far, colour) and each
directional light's colour and direction, and how the game picks them: by
the clock (and blending), at a fixed hour, the number of sets by the hour,
the active set. Show "The game at the hour" lights the scene as
`CMap::GetLightInfo` does at the Map panel's hour: the band's set, blended
into the next over the hour before it, the first light following the sun.
"One light set" shows the chosen set alone; turning one of its suns ("c2
light ...") in the view sets that light's direction. Fog shows in Material
Preview and Rendered (Fog in the Map panel), linear from near to far, over
every material but those whose name flags turn it off (`f0`).

**Animations.** Play Animations runs the map's anime points as the engine
steps them, 25 a second: position, rotation and scale of placements, pieces
and model nodes, in every mode (the clock's hands and "by the hour" read the
Map panel's hour; "look at the player" holds). Colour and alpha are not
shown. Pressing it again stops them and puts everything back, as does
exporting, saving or undoing: a played frame is never written.

**Handles and views.** An invent point shows its box as a cage to drag and
scale, a sound point arrows for its near and far distances. A fixed camera's
Look Through shows the map from it at the engine's 52 degrees, aimed at its
first box; the cameras' boxes and other positions are drawn (Camera boxes),
as are the spots where the map's scripts stand the player (Script spots).
A new mesh's Draw Flags set how the game draws it (blend, alpha test, unlit,
no depth write, fog), written into its node name after `--` as the engine
reads it; the disc's own nodes keep theirs.

**Textures.** Paint on a map picture (Texture Paint, or edit the PNG in
`textures/` and reload it), or give a material another of the map's
pictures or a new image: building puts a changed picture back into the
texture file that holds it, and a picture the disc lacks into the map's own,
named by the image (31 Latin-1 characters at most, sides powers of two up to
1024).

**Add.** Points at the 3D cursor: a position (`point`, for spawns and
doors), door, treasure chest, event trigger, point light, sound emitter,
invent box, effect, fire, lens flare, piece animation (it names the active
model node, piece or placement and turns it about Y, a degree a frame).
They go into the active object's
placement (copied with its template) or, with nothing in a placement active,
the map's own `points`. A door is made as the disc's are (action button, the
screen fading out); its program, the map script's program it runs once open,
is a number or given by name in `mod.json` (`"programs"`, as for a door into
a new map: [editing maps](../docs/guide/maps.md#doors-between-maps)). An event point's
program has a menu of the map script's programs (Read Map Scripts) and "By
name in mod.json", which writes the point's line in `"programs"` for build to
number. Names are made unique within their parent and kept
to 31 bytes. Duplicate Placement copies a placement with its pieces and
points (meshes shared) under a new name (`house01_2`); with no slot it is
appended after the map's placements.

Door to Another Map adds a door at the cursor that goes to a map of the list
(or a new map of the mod): the door point (its target the map), a program in
the mod's patch of this map's script (`doors_MAP.c`, appended to:
`jump_to_map(-1, "TARGET", map_start); WAIT();`, or another program of the
target to arrive at), and the map line's `"programs"` giving the door that
program. The way back is a door in the other map, made the same way.

**Templates of any map.** In the Lay Out panel pick a map, List Templates
(it is exported with `dc2tk map-export` first when it has not been), pick a
template and Place at Cursor: a new placement of a template of its own
(`SOURCE_TEMPLATE`), its pieces and points as the other map has them. A map
reaches only its own packs, so its models become new models of this map
(`SOURCE_MODEL-m.mds` and the rest) and its pictures new pictures
(`SOURCE_PICTURE`, those narrower than 16 scaled up, as most maps' formats
need), which the build adds to the map's model and texture packs.

**New models.** Model something (any meshes, with any of the map's pictures
or new images), select it and press New Model from Selection: it becomes a
new placement (`MAP_NAME`, at the meshes' centre) with a piece of a new model
(`MAP_NAME-m.mds`) holding the meshes where you put them, and, unless you
untick it, a collision copy (`-a`, triangulated, area 6) so the player cannot
walk through it, and if you tick it a camera collision copy (`-c`). Building
adds the models to the map's model pack with their `info.cfg` entries.

**Collision.** Show collision, select a collision mesh, go into Edit Mode,
select faces, set ground, foot sound, area and mask in the Add panel's
Collision box, and press "Set selected faces": the faces get the material
`col_groundG_footF_areaA_maskM`, made when missing. Walkable (Map panel)
tints the shown floor collision green where the player stands, red where the
engine takes a triangle for a wall (its normal less than half up) and blue
for water. Collision from Mesh gives the selected drawn meshes' placement
a collision piece of a new model made from them, triangulated and its flat
areas merged (and camera collision too when asked), replacing the one it
made before. Split Collision cuts the active collision mesh into a node per
square (320 by default), each piece placing the model getting the new nodes,
since every query looks through all the triangles of each node it touches.

**Delete.** Delete a placement with its pieces and points as usual: since
scripts address placements by slot, the build keeps its slot and hides it
(show 0) rather than removing it.

**Template libraries.** A dungeon floor's map (`d01f01`) places none of its
parts: the game lays them out at run time. Its import shows each template
once, in rows by family (`way`, `room`, `door`, ...), as a placement marked
"Template ..., shown once": edit its pieces, collision and points and the
export changes the template; the placement itself places nothing, so moving
it changes nothing (see [dungeons](../docs/internals/dungeons.md), where
area 5 floors are where chests and monsters may stand). Each part's 320 x
320 cell is drawn about its origin, its closed sides (`MOVE_FLAG`) as walls,
which the part's box toggles side by side (-X, -Z, +X, +Z). Preview Dungeon
Layout lays a room of `extracted/data/layouts.json` out from the parts, cell
by cell, as copies beside the map (never exported); room -1 clears it.

**Check.** Validate Map lists errors and warnings (select an entry's object
with its button; the whole list is in the text "dc2tk validation"): names over
31 bytes (cp932), event targets over 15, repeated placement names, more than
320 placements (400 for dungeons), unknown point kinds, collision faces that
are not triangles or whose material is not a `col_` name, more than 8
`TIME_LIGHT_NUM`, nodes without `extras.c2`, animations naming a placement,
piece or model node the scene lacks, picture names over 31 Latin-1
characters, and new pictures whose sides are not powers of two up to 1024;
it notes templates whose placements differ (the build splits them), names
the map's scripts reach that the scene no longer has, and new collision
nodes of more than 512 triangles. The Budgets panel shows the placements
the built map will have (deleted ones keep their slots) against its limit,
the new models and pictures, the triangles drawn and collided with, and the
last preview's memory figures, which are the engine's own sums.

**Play in Game (F5).** Exports the map into the mod, builds the mod into the
native port's override folder (`dc2tk mod watch --once`, a second or two for a
map) and shows it in the debug port: on the map with the player at the 3D
cursor, facing its way, or, with "Where the player is", reloaded with the
player where they stand. The debug port (`python3 -I tools/port.py --movies
--debug`) is started over the retail disc when it is not running, and taken past
the language select and the title by itself; the retail disc must be the ISO.
Edit, press F5 again, and the game shows the change a few seconds later. The
cursor button beside it, Cursor to Player, does the opposite: it puts the 3D
cursor where the player stands in the game and centres the view there. A dungeon
floor's map is reloaded where the player is, since its parts are laid out at
run time.

**Try it.** Read Map Scripts runs `dc2tk map SOURCE NAME --json` and keeps
what the map's scripts know of it in the .blend: their programs, the spots
where they place the player, and the placements, pieces and points they
name (those of other maps that shared scripts name are left out). Preview
Changes exports the map into the mod and runs `dc2tk build --dry-run`: what
the build would make of it (templates split, placements added and deleted,
models rebuilt and added, pictures repainted and added, the memory the map
uses) in a few seconds, without an image. Test Here exports and builds a
test image beside the output one (`NAME-test.iso`) whose New Game starts on
the map with the player at the 3D cursor, facing its way: a mod `test-here`
in the mods folder, rewritten each time, patches the map's script with a
program that places the player, and `--start-at MAP:TEST_HERE` goes there (a
new map of the mod, or one without a script, starts at its `map_start`).
"Boot in PCSX2" adds `--play`.

**A map of your own.** New Map from This One makes the map in the scene a
new map of the mod: a name (lower-case letters, digits and `_`, 12 at most)
and a title, its `add` line in `mod.json` (`"like"` the map it was), a
script of its own whose `map_start` stands the player at the 3D cursor, and
optionally New Game starting there; "Start empty" removes every placement
and point (light sets, fog and cameras stay) and lays a ground with
collision and a start point. Its line has `"compact": true`, so the
placements deleted from it are left out rather than kept hidden in their
slots (no script numbers a new map's placements).

**Export.** Choose the mod folder (`mods/NAME`) and press Export Map to Mod.
After a validation without errors, the map under its `map:` root, and
nothing else in the file, is written (meshes outside the map, and a second
copy of the map in the scene, are named in a warning, since they are left
out) as glTF Separate, +Y up, with custom
properties, to `maps/NAME/NAME.gltf` in the mod (textures in `textures/`),
and `mod.json` gets `"map:NAME": "maps/NAME/NAME.gltf"` under `replace`
(made when missing; other keys kept; written with a 2-space indent, as
dc2tk writes its JSON), unless a line under `add` already names that scene
as its `"file"`: then the export is that new map's, and the disc's map is
left alone. Blender's `.001` suffixes are taken
off node and collision material names in the written file, since pieces and
points repeat in every placement of a template. To make a new map of the
export instead, move its line under `add` with the map it came from:
`"map:m05x": {"file": "maps/m05/m05.gltf", "like": "m05"}`
([editing maps](../docs/guide/maps.md#doors-between-maps)).

**Build.** Set the retail disc, the output ISO and the source tree and press
Build ISO with Mod: `dc2tk build SOURCE DISC -o OUTPUT --mods MODS --mod NAME`
runs in the background, its last lines in the panel and all of it in the
text "dc2tk output". The retail disc is the game's ISO or an untouched
extracted disc folder (`extracted/disc`, which `build` packs into the new
image first); the file browser picks a file, so type or paste a folder.
`build` refuses a map line's scene of the wrong shape (a node's `children`,
a mesh's `primitives` or the materials not a list, a child that is not one
of the nodes) with an error naming it, rather than leaving that part out of
the map.

## Tests

```
blender --background --factory-startup --python blender-plugin/tests/test_addon.py
```

enables the add-on, writes a small map with `tests/make_test_map.py` (two
placements of a template with a model, a collision and a camera-collision
piece, a point and an animation of the model's node; a door, a point light
with a KHR light, a sound, an invent box, an animation of a piece, a point
light without a KHR light, and a door whose `extras.c2` Blender keeps as
text; a fixed camera; map settings), imports it (on Windows through a
stand-in for `dc2tk map-export`), checks the game look, the light set's world
and suns and the hour filter, what the animations move and the lines drawn
for them, edits fields, adds a point of every kind, duplicates a placement,
makes a new model from a mesh, paints a collision face, validates, draws
every panel with a checking stand-in layout, exports into a mod and checks
the written glTF: every node and `extras.c2` of the source kept (edited ones
as edited), transforms unchanged, the new nodes with valid extras, the look
back after the export, `mod.json` updated; then runs `dc2tk build` through the
stand-in. `make_test_map.py OUT_DIR` also runs on its
own with any Python 3.

`test_tools.py` checks the tools on the same map: editing several objects
at once, the outliner, dropping onto the ground, game coordinates, a
template's placements, lighting by the hour and the light set editor (a
turned sun included), animation steps and putting back, the scripts' names
and spots, budgets, handles, dungeon cells, walkability, the test loop (Read
Map Scripts, Preview Changes and Test Here through the stand-in, which
answers `map --json`), the door wizard and programs, the template library,
collision from a mesh and split collision, draw flags, a player for scale, a
dungeon layout, and a new map from the scene.

The fixture includes `extras.c2.name` and the current `map.settings` nesting.
`test_regressions.py` also checks name edits and duplication in exported glTF,
validation against game names, CP932 byte limits, JSON list editing, light
settings, callbacks for every point kind, failed-export cleanup,
and subprocess output and exit codes. These checks run from `test_addon.py`.

## Code layout

`schema.py` holds field dictionaries and format rules. Each point field has a
`key` in the point's `data`, a `label` and a `kind`. Vector fields also specify
`size` and optional element `labels`. `choices` contain a numeric value,
identifier, label, and description. Missing defaults are filled from the
field's type and size.

`c2data.py` reads and writes game metadata. The toolkit reads game names from
`c2.name`, falling back to the Blender name (less its prefix and `.001`) for
nodes without one, such as model nodes. Map settings come from
`c2.map.settings`.
`props.py` declares Blender properties and their named callbacks; editing
functions receive the object they change. Blender registration classes hold
the properties, operators, panels, menus, and preferences the API requires.

`edit.py` creates and edits scene objects. Model naming, placement creation,
piece creation, mesh parenting, and collision copying have separate functions.
`export_mod.py` saves temporary visibility and selection changes in a dictionary
and restores them in `finally`. `look.py` switches material shaders for export
and restores them afterward. `runner.py` stores process state in dictionaries;
its reader thread collects output without calling Blender's API.

`tools.py` lays a map out (ground, snapping, coordinates, a template's
placements, the player for scale); `outliner.py`, `menus.py` and
`budgets.py` are the list, Blender's menus and the Budgets panel;
`lighting.py` edits light sets and keeps turned suns in step; `animate.py`
steps anime points; `scripts.py` keeps the scripts' report in a text block;
`testing.py` is the loop to the game (preview, test image), and `play.py` the
one to the native port (Play in Game, Cursor to Player); `wizards.py`
makes doors, programs and new maps; `library.py` takes templates from other
maps and lays dungeon layouts out; `collision.py`, `views.py` and
`gizmos.py` are the collision tools, cameras and draw flags, and the
handles; `overlay.py` draws everything the scene has no object for.

Enable the add-on through Blender before calling its operators or preferences
from a script. The tests use the same registration path as the application.

## What Blender does to the scene

Worth knowing when reading an exported scene:

- `dc2tk map-export` writes the `.gltf` as compact JSON, its floats in
  their shortest form that reads back to the same value; the add-on's export
  writes it again with a 1-space indent. dc2tk reads either, and a node's
  `extras.c2` as an object or as JSON text.

- Object, mesh and material names are unique in Blender, so repeated names
  get `.001`; the export takes them off again.
- Custom properties hold JSON except integers beyond 32 bits and lists that
  mix text and numbers; a list mixing ints and floats comes back as floats
  (40 as 40.0). When a node's `extras.c2` cannot be stored, Blender's
  importer keeps it as text: the add-on shows it read-only and writes it
  back as JSON on export.
- The exporter writes children in name order and adds `NORMAL` to
  collision meshes; placement order is kept by `slot`.
- It keeps world matrices in 32 bits, so pieces of placements thousands of
  units out come back moved by a few thousandths; `dc2tk build` takes
  positions within 0.005 (and 2e-6 of the value) as unmoved.
- It writes some meshes' colours without alpha, and gives every primitive of
  a mesh with colours a colour (white where there was none); neither counts
  as an edit.
- Textures it has not changed are copied as they are; changed ones are
  written again as PNG and compared by pixels.
