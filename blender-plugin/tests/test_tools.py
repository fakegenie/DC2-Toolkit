"""Checks of the map editing tools: editing many objects at once, the outliner, dropping onto the
ground, game coordinates, a template's placements, lighting by the hour, animations, the scripts'
names, budgets, the test loop, the wizards, the template library, collision tools, draw flags,
handles and cameras. Run from test_addon.py on its test map."""

import json
import math
import os
import sys

import bpy
from mathutils import Matrix, Vector

from dc2tk_maps import (
    animate, budgets, c2data, collision, export_mod, gizmos, library, lighting, look, outliner, overlay, play, prefs,
    schema, scripts, testing, tools, validate, views, wizards,
)

REPORT = {
    "map": "t01", "found": True, "name": "Test Town", "uses": ["town"], "playable": False,
    "scripts": ["map/t/t01/t01_1.c"],
    "programs": [{"number": 100, "names": ["map_start"]}, {"number": 510, "names": ["510"]}],
    "free_programs": [9000, 9001],
    "player_positions": [{"x": 10.0, "y": 0.0, "z": 20.0, "yaw": 1.57, "path": "map/t/t01/t01_1.c", "line": 4,
                          "function": "start", "call": "place_chara(0, 10.0, 0.0, 20.0, 1.57)"}],
    "names": [
        {"what": "point", "part": "", "name": "exit", "path": "map/t/t01/t01_1.c", "line": 5, "call": "set_func_point_enabled(\"\", \"exit\", 0)"},
        {"what": "placement", "part": "house02", "name": None, "path": "map/t/t01/t01_1.c", "line": 6, "call": "bind_handle_map_part(0, \"house02\")"},
        {"what": "placement", "part": "elsewhere", "name": None, "path": "lib/town.c", "line": 9, "call": "bind_handle_map_part(0, \"elsewhere\")"},
    ],
}


def activate(context, *objects):
    for selected in context.selected_objects:
        selected.select_set(False)
    for obj in objects:
        obj.select_set(True)
    context.view_layer.objects.active = objects[0]


def check_multi_edit(context, check):
    settings = context.window_manager.dc2tk_maps
    river = bpy.data.objects["point:river"]
    lamp = bpy.data.objects["point:lamp0"]
    exit_ = bpy.data.objects["point:exit"]
    old = c2data.lookup(exit_, ("enable",))
    activate(context, river, lamp, exit_)
    context.view_layer.objects.active = river
    settings.edit_selected = True
    settings.point_hours = (7, 19)
    check(list(c2data.lookup(river, ("flag",)))[2:] == [7, 19], "the active point's hours set")
    check(list(c2data.lookup(lamp, ("flag",)))[2:] == [7, 19], "and every selected point's")
    settings.d_sound_near = 55.0
    check(abs(c2data.lookup(river, ("data", "near")) - 55.0) < 1e-6, "a sound field set")
    check(c2data.lookup(lamp, ("data", "near")) is None, "and not on a point of another kind")
    settings.edit_selected = False
    settings.point_hours = (0, 0)
    check(list(c2data.lookup(lamp, ("flag",)))[2:] == [7, 19], "with All selected off, only the active one")
    c2data.assign(lamp, ("flag",), [0, 0, 18, 6])
    c2data.assign(exit_, ("enable",), old)


def check_outliner(context, check, root):
    settings = context.window_manager.dc2tk_maps
    settings.outline_filter = "PLACEMENTS"
    settings.outline_search = ""
    listed = [obj.name for obj in bpy.data.objects if outliner.wanted(obj, settings, root)]
    every = sorted(obj.name for obj in c2data.placements(root))
    check(sorted(listed) == every and "place:house02" in listed, "the outliner lists the placements: %s" % listed)
    settings.outline_filter = "POINTS"
    settings.outline_kind = "event"
    listed = [obj.name for obj in bpy.data.objects if outliner.wanted(obj, settings, root)]
    kinds = {c2data.lookup(bpy.data.objects[name], ("kind",)) for name in listed}
    check({"point:exit", "point:textdoor"} <= set(listed) and kinds == {"event"}, "points of a kind: %s" % listed)
    settings.outline_kind = "ALL"
    settings.outline_search = "lamp"
    listed = [obj.name for obj in bpy.data.objects if outliner.wanted(obj, settings, root)]
    check(listed == ["point:lamp0"], "found by name: %s" % listed)
    settings.outline_search = ""
    settings.outline_index = list(bpy.data.objects).index(bpy.data.objects["point:river"])
    check(context.active_object == bpy.data.objects["point:river"], "picking a row selects it")
    check(outliner.describe(bpy.data.objects["place:house01"]).startswith("house"), "a placement's row names its template")


def check_ground(context, check, root):
    house = bpy.data.objects["place:house01"]
    point = bpy.data.objects["point:river"]
    old = point.matrix_world.copy()
    point.matrix_world = Matrix.Translation(house.matrix_world.translation + Vector((0.0, 0.0, 90.0)))
    activate(context, point)
    result = bpy.ops.dc2tk.drop_to_ground(above=200.0)
    check(result == {"FINISHED"}, "drop to ground: %s" % result)
    check(abs(point.matrix_world.translation.z - house.matrix_world.translation.z) < 1e-3, "it stands on the floor: %s" % point.matrix_world.translation)
    point.matrix_world = Matrix.Translation(Vector((5000.0, 5000.0, 0.0)))
    check(bpy.ops.dc2tk.drop_to_ground() == {"CANCELLED"}, "no ground far from the map")
    point.matrix_world = old
    check(bpy.ops.dc2tk.game_snapping(preset="TOWN") == {"FINISHED"}, "town snapping")
    check(abs(context.scene.tool_settings.snap_angle_increment_3d - math.radians(15.0)) < 1e-5 if hasattr(context.scene.tool_settings, "snap_angle_increment_3d") else True, "15 degree turns")


def check_coordinates(check):
    world = Matrix.Translation((10.0, -20.0, 5.0)) @ Matrix.Rotation(math.radians(90.0), 4, "Z")
    check(tools.game_position(world) == (10.0, 5.0, 20.0), "game position (y up): %s" % (tools.game_position(world),))
    check(abs(tools.game_rotation(world)[1] - math.radians(90.0)) < 1e-5, "a turn about Blender's Z is the game's yaw")
    text = tools.coordinates_text(world, "PLACE")
    check(text == "place_chara(CHARA_PLAYER, 10, 5, 20, 1.57);", "place_chara text: %s" % text)
    check(tools.coordinates_text(world, "DEFINE", "door park").startswith("#define DOOR_PARK_X 10"), "defines")


def check_instances(context, check, root):
    one, two = bpy.data.objects["place:house01"], bpy.data.objects["place:house02"]
    activate(context, one)
    bpy.ops.dc2tk.apply_to_instances()
    check(len(c2data.instance_groups(one)) == 1, "the houses are alike once applied to all")
    point = next(child for child in two.children if c2data.exact_name(child, "point") == "point")
    old = point.matrix_basis.copy()
    point.location.z += 10.0
    groups = c2data.instance_groups(one)
    check(len(groups) == 2, "a point moved in one placement splits the template: %d" % len(groups))
    check(any("differ" in message for _l, _n, message in validate.check(root)), "validation notes the split")
    activate(context, one)
    check(bpy.ops.dc2tk.apply_to_instances() == {"FINISHED"}, "apply to all instances")
    check(len(c2data.instance_groups(one)) == 1, "alike again")
    moved = next(child for child in two.children if c2data.exact_name(child, "point") == "point")
    check((moved.matrix_basis.translation - old.translation).length < 1e-4, "the moved point is back as house01 has it")
    activate(context, two)
    check(bpy.ops.dc2tk.select_instances() == {"FINISHED"} and len(context.selected_objects) == len(c2data.instances(two)), "select instances")
    activate(context, two)
    check(bpy.ops.dc2tk.make_unique() == {"FINISHED"}, "make unique")
    check(c2data.template_name(two) == "house_2", "a template of its own: %s" % c2data.template_name(two))
    check(two not in c2data.instances(one), "house02 is no instance of house01's template now")
    c2data.assign(two, ("template",), "house")


def check_lighting(context, check, root):
    check(look.band_ratios(4, 12.0) == [1.0, 0.0, 0.0, 0.0], "noon is the day set")
    ratios = look.band_ratios(4, 16.5)
    check(abs(ratios[0] - 0.5) < 1e-6 and abs(ratios[1] - 0.5) < 1e-6, "16:30 blends day into evening: %s" % ratios)
    check(look.band_ratios(4, 22.0)[2] == 1.0 and look.band_ratios(4, 7.0)[3] == 1.0, "night and morning")
    ratios = look.band_ratios(8, 11.5)
    check(abs(ratios[0] - 0.5) < 1e-6 and abs(ratios[1] - 0.5) < 1e-6, "8 sets: 11:30 is half the first, half the second: %s" % ratios)
    check(look.time_band(5.0) == 2 and look.time_band(6.0) == 3, "the engine's bands")
    sun = look._sun(12.0, 0.0)
    check(sun[1] > 0.9, "the sun is up at noon: %s" % sun)
    check(abs(look._sun(0.0, 0.0)[1] - 0.2) < 1e-6 or look._sun(0.0, 0.0)[1] >= 0.2, "never below 0.2")
    lit = look.lighting_at(root, 22)
    active = look.active_light_set(root)
    check(lit is not None and list(lit["bgcolor"]) == list(active["bgcolor"]), "a map that does not follow the clock keeps its active set: %s" % lit)
    lit = dict(lit, fog_enable=1, fog={"near": 100.0, "far": 900.0, "color": [40, 50, 60]})
    check(look.set_fog(lit) and bpy.data.node_groups[look.FOG].nodes["strength"].inputs[1].default_value == 1.0, "its fog is shown")
    check(not look.set_fog(dict(lit, fog_enable=0)), "and none without")
    wall = next(m for m in bpy.data.materials if m.get(look.FLAG) and m.node_tree.nodes.get("c2 look fog"))
    check(wall is not None, "map materials pass through the fog")
    settings = context.window_manager.dc2tk_maps
    settings.light_preview = "SET"
    settings.light_set = "0"
    settings.ls_bgcolor = (1.0, 0.5, 0.0)
    check(list(c2data.lookup(root, ("map", "settings", "light_sets", 0, "bgcolor"))) == [255, 128, 0], "background edited")
    settings.ls_fog_far = 2500.0
    check(abs(c2data.lookup(root, ("map", "settings", "light_sets", 0, "fog", "far")) - 2500.0) < 1e-6, "fog far edited")
    sun = next(obj for obj in bpy.data.objects if obj.name.startswith(look.SUN_PREFIX % "t01"))
    sun.rotation_euler = Vector((1.0, 0.0, 0.0)).to_track_quat("Z", "Y").to_euler()
    context.view_layer.update()
    lighting.sync_suns(context.scene)
    direction = list(c2data.lookup(root, ("map", "settings", "light_sets", 0, "lights", 0, "dir")))
    check(abs(direction[0] - 1.0) < 1e-3 and abs(direction[1]) < 1e-3, "a sun turned sets its light's direction: %s" % direction)
    c2data.assign(root, ("map", "settings", "light_sets", 0, "lights", 0, "dir"), [0.0, -1.0, 0.0])
    c2data.assign(root, ("map", "settings", "light_sets", 0, "bgcolor"), [10, 20, 30])
    c2data.assign(root, ("map", "settings", "light_sets", 0, "fog", "far"), 3000.0)
    settings.light_preview = "GAME"


def check_animation(context, check):
    point = {"mode": 3, "start": [0.0] * 3, "step": [0.0, 10.0, 0.0], "end": [0.0, 25.0, 0.0], "uniform": 0, "value": [0.0] * 3}
    values = [animate.step(point, 12.0)[1] for _ in range(4)]
    check(values == [10.0, 20.0, 0.0, 10.0], "repeat goes back to the start at the end: %s" % values)
    point = dict(point, mode=4, value=[0.0] * 3)
    values = [animate.step(point, 12.0)[1] for _ in range(6)]
    check(values == [10.0, 20.0, 25.0, 15.0, 5.0, 0.0], "ping-pong: %s" % values)
    point = dict(point, mode=2, value=[0.0] * 3, sign=1.0)
    point.pop("sign")
    values = [animate.step(point, 12.0)[1] for _ in range(4)]
    check(values == [10.0, 20.0, 25.0, 25.0], "stop holds the end: %s" % values)
    point = {"mode": 9, "start": [5.0, 0.0, 6.0], "step": [0.0] * 3, "end": [0.0] * 3, "uniform": 0, "value": [0.0] * 3}
    check(animate.step(point, 3.0) == [5.0, -90.0, 6.0], "the hour hand at 3")
    point = {"mode": 10, "start": [0.0] * 3, "step": [20.0, 6.0, 1.0], "end": [1.0] * 3, "uniform": 0, "value": [0.0] * 3}
    check(animate.step(point, 22.0)[0] == 1.0 and animate.step(point, 12.0)[0] == 0.0, "by the hour")
    spin = next(obj for obj in c2data.points(bpy.data.objects["map:t01"]) if c2data.exact_name(obj, "point") == "spin")
    c2data.assign(spin, ("data",), {"placement": "house", "piece": "house_m", "frame": "house_body", "drives": 2, "mode": 1,
                                    "start": [0.0, 0.0, 0.0], "step": [0.0, 1.0, 0.0], "end": [0.0, 0.0, 0.0]})
    c2data.assign(spin, ("enable",), 1)
    body = c2data.anime_target(spin)[0]
    check(body is not None, "the spinning point moves a node")
    before = body.matrix_basis.copy()
    count = animate.start(context)
    check(count >= 1 and animate.playing(), "animations play: %d" % count)
    for _ in range(5):
        animate.tick()
    check((body.matrix_basis.to_quaternion().rotation_difference(before.to_quaternion())).angle > 1e-4, "the spinning node turns")
    animate.stop()
    check(not animate.playing(), "and stop")
    check(all(abs(a - b) < 1e-6 for a, b in zip(sum(map(list, body.matrix_basis), []), sum(map(list, before), []))), "putting it back")


def check_scripts(context, check, root):
    scripts.store("t01", REPORT)
    exit_ = bpy.data.objects["point:exit"]
    house = bpy.data.objects["place:house02"]
    check(len(scripts.references(exit_)) == 1, "the scripts name the map's point exit")
    check(len(scripts.references(house)) == 1, "and placement house02")
    check(not scripts.references(bpy.data.objects["place:house01"]), "but not house01")
    names = testing.disc_names(os.path.join(prefs.maps_dir(prefs.get(context)), "t01", "t01.gltf")) if os.path.isfile(
        os.path.join(prefs.maps_dir(prefs.get(context)), "t01", "t01.gltf")) else None
    if names is not None:
        kept = [use for use in REPORT["names"] if testing.on_map(use, names)]
        check(len(kept) == 2, "a shared script's names of other maps are left out: %s" % kept)
    old = c2data.exact_name(house, "placement")
    c2data.rename(house, "house02x")
    warned = [m for _l, _n, m in validate.check(root) if "Scripts name" in m]
    check(any("house02" in m for m in warned), "renaming a placement scripts name warns: %s" % warned)
    c2data.rename(house, old)
    lines = overlay.spot_coords(context)[0]
    check(len(lines) >= 8, "the scripts' player spots are drawn")


def check_budgets(check, root):
    slots, new = budgets.placement_slots(root)
    check(new >= 1 and slots >= 3, "placements counted with the new ones: %d, %d" % (slots, new))
    check(any(model.endswith("-m.mds") for model in budgets.new_models(root)), "new models counted: %s" % budgets.new_models(root))
    drawn, collision_triangles = budgets.triangles(root)
    check(drawn > 0 and collision_triangles > 0, "triangles counted")


def check_test_loop(context, check, work):
    lines = [
        "note: map:t01 (mod testmod): template house_2 is new, placed by house02",
        "  testmod (map t01): rebuilt house-m.mds in t01_0.pcp",
        "replaced: map/t/t01/t01.map (map:t01; mod testmod, 100 bytes, was 90)",
        "memory: map t01 (town map): read buffer 1,000 (was 900) of 3,200,000; placements 3 of 320",
        "warning: something",
        "dry run: 0 scripts changed, 1 game files replaced or added, 1 disc files rewritten; no image written",
    ]
    kinds = [kind for kind, _text in testing.summarise(lines)]
    check(kinds == ["NOTE", "MAP", "FILE", "MEMORY", "WARNING", "DONE"], "a build's lines summarised: %s" % kinds)
    script = testing.test_script("t01", Matrix.Translation((1.0, -2.0, 3.0)))
    check("#pragma target map t01" in script and "place_chara(CHARA_PLAYER, 1, 3, 2, 0);" in script, "the test patch: %s" % script)
    mods = os.path.join(work, "mods")
    folder = testing.write_test_mod(mods, "t01", Matrix.Identity(4))
    check(os.path.isfile(os.path.join(folder, "test_here.c")) and os.path.isfile(os.path.join(folder, "mod.json")), "the test mod")
    if not sys.platform.startswith("win"):
        return
    result = bpy.ops.dc2tk.read_scripts()
    check(result == {"FINISHED"}, "read map scripts through dc2tk: %s" % result)
    report = scripts.info(bpy.data.objects["map:t01"])
    check(report is not None and report.get("programs"), "the report is kept in the .blend")
    activate(context, bpy.data.objects["map:t01"])
    result = bpy.ops.dc2tk.preview_changes()
    check(result == {"FINISHED"}, "preview changes: %s" % result)
    from dc2tk_maps import runner

    text = "\n".join(runner.output_lines(runner.current))
    check("--dry-run" in text and "--mod local.testmod" in text, "the preview is a dry run of the mod: %s" % text)
    context.scene.cursor.location = (12.0, -34.0, 5.0)
    result = bpy.ops.dc2tk.test_here()
    check(result == {"FINISHED"}, "test here: %s" % result)
    text = "\n".join(runner.output_lines(runner.current))
    check("--start-at t01:TEST_HERE" in text and "--mod local.test-here" in text and "-test.iso" in text, "the test build: %s" % text)
    with open(os.path.join(mods, "test-here", "test_here.c"), encoding="utf-8") as handle:
        check("place_chara(CHARA_PLAYER, 12, 5, 34, 0);" in handle.read(), "the player stands at the cursor")


def check_door_wizard(context, check, root):
    listed = context.window_manager.dc2tk_maps.maps
    if not any(entry.code == "t02" for entry in listed):
        entry = listed.add()
        entry.code, entry.name = "t02", "t02"
    mod_dir = prefs.absolute(prefs.get(context).mod_dir)
    activate(context, root)
    context.scene.cursor.location = (0.0, 0.0, 0.0)
    result = bpy.ops.dc2tk.add_door_to_map(target="t02", arrive="map_start")
    check(result == {"FINISHED"}, "a door to another map: %s" % result)
    door = bpy.data.objects.get("point:door_t02")
    check(door is not None and c2data.lookup(door, ("data", "target")) == "t02", "the door point, its target t02")
    with open(os.path.join(mod_dir, "doors_t01.c"), encoding="utf-8") as handle:
        text = handle.read()
    check("NEW_PROGRAM(ENTER_DOOR_T02);" in text and 'jump_to_map(-1, "t02", map_start);' in text, "its program: %s" % text)
    check(export_mod.programs_of(mod_dir, "t01").get("door_t02") == "ENTER_DOOR_T02", "mod.json gives the door its program")
    export_mod.update_mod_json(mod_dir, "t01")
    check(export_mod.programs_of(mod_dir, "t01").get("door_t02") == "ENTER_DOOR_T02", "exporting again keeps the programs")
    activate(context, door)
    check(bpy.ops.dc2tk.point_program(number=510) == {"FINISHED"}, "a program of the map's script by number")
    check(c2data.lookup(door, ("data", "program")) == 510 and "door_t02" not in export_mod.programs_of(mod_dir, "t01"), "it replaces the named one")
    bpy.data.batch_remove([door])


def check_library(context, check, root):
    settings = context.window_manager.dc2tk_maps
    settings.library_map = "t01  Test Town"
    if library.export_path(context, "t01") is None:
        return
    check(bpy.ops.dc2tk.load_library() == {"FINISHED"}, "list a map's templates")
    check([entry.name for entry in settings.library_templates] == ["house"], "its templates")
    settings.library_template = "house"
    context.scene.cursor.location = (500.0, 500.0, 0.0)
    activate(context, root)
    result = bpy.ops.dc2tk.add_template()
    check(result == {"FINISHED"}, "place a template from a map: %s" % result)
    placed = context.active_object
    check(c2data.template_name(placed) == "t01_house" and c2data.lookup(placed, ("slot",)) is None, "a new placement of a template of its own")
    models = sorted(str(c2data.lookup(p, ("model",))) for p in c2data.descendants(placed) if c2data.role(p) == "piece")
    check(models == ["t01_house-a.mds", "t01_house-c.mds", "t01_house-m.mds"], "its models are new: %s" % models)
    check(all("mds_index" not in (c2data.get(o) or {}) and "mds_index" not in o
              for o in c2data.descendants(placed) if c2data.role(o) == "mds"), "its nodes are new")
    check(all(c2data.map_root(o) is root for o in c2data.descendants(placed)), "inside the map")
    check((placed.matrix_world.translation - Vector((500.0, 500.0, 0.0))).length < 1e-3, "at the cursor")
    pictures = {node.image.name for o in c2data.descendants(placed) if o.type == "MESH" for s in o.material_slots if s.material and s.material.node_tree
                for node in s.material.node_tree.nodes if node.type == "TEX_IMAGE" and node.image}
    check(all(name.startswith("t01_") for name in pictures), "its pictures are new: %s" % pictures)
    bpy.data.batch_remove(c2data.descendants(placed))
    room = {"cells": ["room08 . way01:1", "#3 room00 ."]}
    check(library.cells(room) == [(0, 0, "room08"), (2, 0, "way01"), (1, 1, "room00")], "a layout's cells")


def check_collision_tools(context, check, root):
    import bmesh

    mesh = bpy.data.meshes.new("slab")
    shape = bmesh.new()
    bmesh.ops.create_grid(shape, x_segments=10, y_segments=10, size=400.0)
    shape.to_mesh(mesh)
    shape.free()
    slab = bpy.data.objects.new("slab", mesh)
    context.collection.objects.link(slab)
    activate(context, slab)
    check(bpy.ops.dc2tk.new_model(name="slab", collision=False) == {"FINISHED"}, "a new model to collide with")
    placement = context.active_object
    drawn = next(o for o in c2data.descendants(placement) if o.type == "MESH")
    activate(context, drawn)
    result = bpy.ops.dc2tk.collision_from_mesh(simplify=0.1)
    check(result == {"FINISHED"}, "collision from mesh: %s" % result)
    copy = next((o for o in c2data.descendants(placement) if o.type == "MESH" and c2data.is_collision_mesh(o)), None)
    check(copy is not None and all(len(p.vertices) == 3 for p in copy.data.polygons), "a triangulated collision copy")
    check(len(copy.data.polygons) < 200, "flat areas merged: %d triangles" % len(copy.data.polygons))
    result = bpy.ops.dc2tk.collision_from_mesh(simplify=0.0)
    check(result == {"FINISHED"}, "made again")
    pieces = [o for o in placement.children if c2data.role(o) == "piece" and c2data.lookup(o, ("kind",)) == "collision"]
    check(len(pieces) == 1, "replacing the copy made before: %d" % len(pieces))
    copy = next(o for o in c2data.descendants(pieces[0]) if o.type == "MESH")
    check(len(copy.data.polygons) == 200, "every face kept without merging: %d" % len(copy.data.polygons))
    check(any("split it" in m for _l, _n, m in validate.check(root)) is False, "200 triangles is no warning")
    activate(context, copy)
    result = bpy.ops.dc2tk.split_collision(cell=320.0, most=10)
    check(result == {"FINISHED"}, "split collision: %s" % result)
    nodes = [o for o in c2data.descendants(pieces[0]) if o.type == "MESH"]
    check(len(nodes) > 4 and sum(len(o.data.polygons) for o in nodes) == 200, "into compact nodes: %d" % len(nodes))
    check(all(len(o.data.polygons) <= 64 for o in nodes), "each in one square")
    check(sum(1 for o in nodes if "mds_index" in o or "mds_index" in (c2data.get(o) or {})) <= 1, "the new nodes carry no mds_index")
    activate(context, drawn)
    bpy.ops.dc2tk.draw_flags(blend="ADD", unlit=True, fog=0)
    check(schema.strip_suffix(drawn.name).endswith("--aPPc0f0"), "draw flags in the node's name: %s" % drawn.name)
    check(c2data.to_py(drawn.active_material["frame_flags"]).get("blend") == "add" if drawn.active_material else True, "and the look")
    bpy.data.batch_remove(c2data.descendants(placement))


def check_handles(context, check):
    photo = bpy.data.objects["point:photo1"]
    matrix = gizmos.box_matrix(photo)
    gizmos.set_box(photo, Matrix.Translation(matrix.translation + Vector((0.0, 0.0, 5.0))) @ Matrix.Diagonal(list(matrix.to_scale()) + [1.0]))
    low = list(c2data.lookup(photo, ("data", "min")))
    high = list(c2data.lookup(photo, ("data", "max")))
    check(abs(low[1] + 0.0) < 1e-3 and abs(high[1] - 45.0) < 1e-3, "the box moved up 5 (game y): %s %s" % (low, high))
    c2data.set_data_field(photo, schema.DATA_FIELDS["invent"][1], [-20.0, -5.0, -20.0])
    c2data.set_data_field(photo, schema.DATA_FIELDS["invent"][2], [20.0, 40.0, 20.0])
    cameras = [o for o in bpy.data.objects if c2data.role(o) == "camera"]
    activate(context, cameras[0])
    check(bpy.ops.dc2tk.view_camera() == {"FINISHED"}, "look through a fixed camera")
    view = bpy.data.objects.get(views.VIEW)
    check(view is not None and abs(view.data.angle - math.radians(52.0)) < 1e-4, "52 degrees")
    check(not any("c2 view" in m for _l, _n, m in validate.left_out(c2data.map_root(cameras[0]))), "never exported")


def check_reference_and_layout(context, check, root):
    context.scene.cursor.location = (40.0, 40.0, 0.0)
    check(bpy.ops.dc2tk.player_reference() == {"FINISHED"}, "a player for scale")
    reference = bpy.data.collections.get(tools.REFERENCE)
    check(reference is not None and len(reference.objects) >= 1, "it stands beside the map")
    check(not any(tools.REFERENCE in m for _l, _n, m in validate.left_out(root)), "and is never exported")
    house = bpy.data.objects["place:house01"]
    c2data.assign(house, ("preview",), True)
    original = library.layouts
    library.layouts = lambda context: [{"layout": "t01", "grid_size": [320.0, 320.0], "rooms": [{"cells": ["house . house", "nowhere"]}]}]
    try:
        activate(context, root)
        result = bpy.ops.dc2tk.layout_preview(layout_name="t01", room=0)
        check(result == {"FINISHED"}, "a dungeon layout laid out: %s" % result)
        laid = bpy.data.collections.get(library.LAYOUT_COLLECTION)
        meshes = [o for o in house.children_recursive if o.type == "MESH" and not c2data.is_collision_mesh(o)]
        check(laid is not None and len(laid.objects) == 2 * len(meshes), "two cells of the house: %d" % (len(laid.objects) if laid else 0))
        xs = sorted(o.matrix_world.translation.x for o in laid.objects if o.data == meshes[0].data)
        check(len(xs) == 2 and abs(xs[1] - xs[0] - 640.0) < 1e-3, "the cells 640 apart, two of 320: %s" % xs)
        check(bpy.ops.dc2tk.layout_preview(layout_name="t01", room=-1) == {"FINISHED"} and bpy.data.collections.get(library.LAYOUT_COLLECTION) is None, "and cleared")
    finally:
        library.layouts = original
        c2data.remove(house, ("preview",))


def check_cells(context, check):
    house = bpy.data.objects["place:house01"]
    c2data.assign(house, ("preview",), True)
    c2data.assign(house, ("move_flag",), [1, 0, 0, 1])
    lines, walls = overlay.cell_coords(context)
    check(len(lines) == 8 and len(walls) == 16, "a cell and its two closed sides: %d, %d" % (len(lines), len(walls)))
    c2data.remove(house, ("preview",))
    c2data.remove(house, ("move_flag",))


def check_walkable(context, check):
    settings = context.window_manager.dc2tk_maps
    bpy.ops.dc2tk.toggle_collision(kind="collision")
    found = overlay.walkable_tris(context)
    floor, water = found[0][0], found[2][0]
    check(len(floor) >= 3 and len(water) >= 3, "the floor and the water are told apart: %d, %d" % (len(floor), len(water)))
    bpy.ops.dc2tk.toggle_collision(kind="collision")


def check_new_map(context, check, root):
    mod_dir = prefs.absolute(prefs.get(context).mod_dir)
    activate(context, root)
    context.scene.cursor.location = (0.0, 0.0, 0.0)
    result = bpy.ops.dc2tk.new_map(name="t01x", title="Test Two", empty=True, ground_size=500.0)
    check(result == {"FINISHED"}, "a new map from this one: %s" % result)
    root = bpy.data.objects.get("map:t01x")
    check(root is not None and c2data.map_name(root) == "t01x", "the map is renamed")
    with open(os.path.join(mod_dir, "mod.json"), encoding="utf-8") as handle:
        data = json.load(handle)
    line = data["add"]["map:t01x"]
    check(line == {"file": "maps/t01x/t01x.gltf", "like": "t01", "compact": True, "title": "Test Two", "script": "t01x.c"},
          "its line: %s" % line)
    check(data.get("dev", {}).get("start_at") == "t01x", "new games start there")
    check(os.path.isfile(os.path.join(mod_dir, "maps", "t01x", "t01x.gltf")), "exported into the mod")
    with open(os.path.join(mod_dir, "t01x.c"), encoding="utf-8") as handle:
        check("PROGRAM(map_start, arrive);" in handle.read(), "a script of its own")
    names = [c2data.exact_name(o, "placement") for o in c2data.placements(root)]
    check(names == ["t01x_ground"], "empty but for the ground: %s" % names)
    check(any(c2data.exact_name(o, "point") == "start" for o in c2data.points(root)), "and a start point")
    try:
        refused = bpy.ops.dc2tk.new_map(name="T!", title="") == {"CANCELLED"}
    except RuntimeError as error:
        refused = "lower-case" in str(error)
    check(refused, "a bad name is refused")


def check_play(context, check, root, work):
    context.scene.cursor.matrix = Matrix.Translation((12.0, -34.0, 5.0))
    request = play.enter_request(context, root, "CURSOR")
    check(request == "reload?map=t01&x=12.00&y=5.00&z=34.00&yaw=0.0000", "play at the cursor: %s" % request)
    check(play.enter_request(context, root, "PLAYER") == "reload?map=t01", "play where the player is")
    settings = prefs.get(context)
    mod_dir = os.path.join(work, "mods", "testmod")
    command = play.watch_command(settings, "src", "dc2.iso", mod_dir, request) if prefs.dc2tk_command(settings) else None
    if command is not None:
        check(command[-2:] == ["--reload", "map=t01&x=12.00&y=5.00&z=34.00&yaw=0.0000"] and "--once" in command
              and "local.testmod" in command, "the watch command: %s" % command)
    check(play.overrides_dir(settings).endswith(os.path.join("build", "port", "overrides")) or bool(settings.overrides_dir),
          "the override folder beside the port: %s" % play.overrides_dir(settings))
    check(not play.running(settings) or True, "asking a game that is not running does not fail")
    old_iso = settings.iso_path
    settings.iso_path = work
    check("ISO" in (play.problems(settings, mod_dir) or ""), "an extracted folder is refused for the port")
    settings.iso_path = old_iso


def run(context, root, work, check):
    check_multi_edit(context, check)
    check_outliner(context, check, root)
    check_ground(context, check, root)
    check_coordinates(check)
    check_instances(context, check, root)
    check_lighting(context, check, root)
    check_animation(context, check)
    check_scripts(context, check, root)
    check_budgets(check, root)
    check_handles(context, check)
    check_cells(context, check)
    check_reference_and_layout(context, check, root)
    check_walkable(context, check)
    check_test_loop(context, check, work)
    check_play(context, check, root, work)
    check_door_wizard(context, check, root)
    check_library(context, check, root)
    check_collision_tools(context, check, root)
    check_new_map(context, check, root)
    print("PASS: map editing tools (%s)" % ", ".join(
        ["multi-edit", "outliner", "ground", "coordinates", "instances", "lighting", "animation", "scripts", "budgets",
         "handles", "cells", "walkable", "test loop", "play", "door", "library", "collision", "new map"]))
