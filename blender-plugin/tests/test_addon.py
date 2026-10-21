import json
import math
import os
import shutil
import sys
import tempfile
import traceback

import bpy
from mathutils import Vector

HERE = os.path.dirname(os.path.abspath(__file__))
ADDON_PARENT = os.path.dirname(HERE)
sys.path.insert(0, ADDON_PARENT)
sys.path.insert(0, HERE)

import make_test_map

failures = []
checks = 0


def check(condition, message):
    global checks
    checks += 1
    if not condition:
        failures.append(message)
        print("FAIL:", message)


def same(a, b, tolerance=1e-4):
    if isinstance(a, bool) or isinstance(b, bool):
        return bool(a) == bool(b) and type(a) in (bool, int) and type(b) in (bool, int)
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= tolerance * max(1.0, abs(a))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(same(a[k], b[k], tolerance) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same(x, y, tolerance) for x, y in zip(a, b))
    return a == b


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def local_matrix(node):
    if "matrix" in node:
        m = node["matrix"]
        return [[m[c * 4 + r] for c in range(4)] for r in range(4)]
    tx, ty, tz = node.get("translation", [0, 0, 0])
    x, y, z, w = node.get("rotation", [0, 0, 0, 1])
    sx, sy, sz = node.get("scale", [1, 1, 1])
    r = [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ]
    return [
        [r[0][0] * sx, r[0][1] * sy, r[0][2] * sz, tx],
        [r[1][0] * sx, r[1][1] * sy, r[1][2] * sz, ty],
        [r[2][0] * sx, r[2][1] * sy, r[2][2] * sz, tz],
        [0, 0, 0, 1],
    ]


def walk(gltf):
    nodes = gltf["nodes"]
    found = {}
    identity = [[1 if i == j else 0 for j in range(4)] for i in range(4)]

    def visit(indices, parent_key, parent_matrix):
        seen = {}
        for index in indices:
            node = nodes[index]
            name = node.get("name", "")
            seen[name] = seen.get(name, 0) + 1
            key = parent_key + ((name, seen[name]),)
            world = mat_mul(parent_matrix, local_matrix(node))
            found[key] = (node, world)
            visit(node.get("children", []), key, world)

    scene = gltf["scenes"][gltf.get("scene", 0)]
    visit(scene["nodes"], (), identity)
    return found


def key_name(key):
    return "/".join(name for name, _ in key)


def enable_addon():
    import addon_utils

    module = addon_utils.enable("dc2tk_maps", default_set=True, persistent=False, handle_error=None)
    if module is None or "dc2tk_maps" not in bpy.context.preferences.addons:
        raise RuntimeError("Could not enable dc2tk_maps")
    return module, True


def write_fake_dc2tk(folder, source_dir):
    import test_tools

    report = os.path.join(folder, "map_report.json")
    with open(report, "w", encoding="utf-8") as handle:
        json.dump(test_tools.REPORT, handle)
    path = os.path.join(folder, "fake_dc2tk.bat")
    with open(path, "w", newline="\r\n") as handle:
        handle.write(
            "@echo off\n"
            'if "%~1"=="map" (\n'
            '  type "' + report + '"\n'
            "  exit /b 0\n"
            ")\n"
            'if "%~1"=="map-export" if "%~2"=="--list" (\n'
            "  echo t01\tTest Town\n"
            "  echo t02\t\n"
            "  exit /b 0\n"
            ")\n"
            'if "%~1"=="map-export" (\n'
            '  xcopy /E /I /Y /Q "' + source_dir + '" "%~4" >nul\n'
            "  echo exported %~2 to %~4\n"
            "  exit /b 0\n"
            ")\n"
            "echo dc2tk %*\n"
            "exit /b 0\n"
        )
    return path


def main():
    work = tempfile.mkdtemp(prefix="dc2tk_maps_test_")
    print("Working in", work)
    source_dir = os.path.join(work, "source")
    source_path = make_test_map.write(source_dir)
    with open(source_path, encoding="utf-8") as handle:
        source = json.load(handle)

    dc2tk_maps, through_prefs = enable_addon()
    from dc2tk_maps import c2data, edit, export_mod, import_map, prefs, schema, validate

    print("Blender", bpy.app.version_string, "- add-on enabled through preferences:", through_prefs)
    settings_prefs = prefs.get()
    context = bpy.context
    wm = context.window_manager
    settings = wm.dc2tk_maps

    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)

    if sys.platform.startswith("win"):
        settings_prefs.dc2tk_path = write_fake_dc2tk(work, source_dir)
        settings_prefs.maps_dir = os.path.join(work, "maps")
        # the map list, from dc2tk map-export --list: a map with a title, and one without
        result = bpy.ops.dc2tk.load_maps()
        check(result == {"FINISHED"}, "the map list loads: %s" % result)
        listed = [(entry.name, entry.code, entry.title) for entry in settings.maps]
        check(listed == [("t01  Test Town", "t01", "Test Town"), ("t02", "t02", "")], "the map list: %s" % listed)
        check(import_map.map_code("t01  Test Town") == "t01" and import_map.map_code(" s17 ") == "s17", "a map's code")
        # a map picked from the list is imported by its code
        result = bpy.ops.dc2tk.import_map(map_name="t01  Test Town")
        check(result == {"FINISHED"}, "import through dc2tk finished: %s" % result)
        check(os.path.isfile(os.path.join(work, "maps", "t01", "t01.gltf")), "dc2tk wrote maps/t01/t01.gltf")
    else:
        result = bpy.ops.dc2tk.import_map_gltf(filepath=source_path)
        check(result == {"FINISHED"}, "import finished: %s" % result)

    root = bpy.data.objects.get("map:t01")
    check(root is not None, "map:t01 imported")
    if root is None:
        return work
    check(c2data.role(root) == "map", "root role is map")
    as_text = sorted(obj.name for obj in bpy.data.objects if c2data.is_stringified(obj))
    check(as_text == ["point:textdoor"], "only the mixed-list point is kept as text: %s" % as_text)
    text_door = bpy.data.objects["point:textdoor"]
    check(c2data.lookup(text_door, ("data", "kind")) == "door", "text extras read back")
    check(text_door.empty_display_type == "CUBE" and abs(text_door.empty_display_size - 15.0) < 1e-6, "text door size 15")
    glow = bpy.data.objects["point:glow"]
    check(glow.type == "LIGHT", "a plight without a KHR light becomes a light")
    check(abs(glow.data.color[1] - 128 / 255) < 1e-4, "its colour from the data")

    collision = [obj for obj in c2data.collision_meshes(root)]
    check(len(collision) == 4, "4 collision meshes (2 placements x collision and camera): %d" % len(collision))
    for obj in collision:
        collections = [c.name for c in obj.users_collection]
        check(
            any(name.startswith("t01 collision") or name.startswith("t01 camera collision") for name in collections),
            "%s is in a collision collection (%s)" % (obj.name, collections),
        )
        check(not obj.visible_get(), "%s is hidden" % obj.name)
    floor = next(obj for obj in collision if schema.strip_suffix(obj.name) == "floor")
    names = sorted(slot.material.name for slot in floor.material_slots)
    check(
        names == ["col_ground0_foot2_area6_mask0", "col_ground0_foot5_area1_mask12"],
        "collision material names kept: %s" % names,
    )
    water = bpy.data.materials["col_ground0_foot5_area1_mask12"]
    check(tuple(round(c, 2) for c in water.diffuse_color[:3]) == (0.15, 0.35, 0.95), "water area coloured blue")

    from dc2tk_maps import look

    wall = bpy.data.materials.get("house_wall")
    check(wall is not None and wall.get(look.FLAG) == 1, "the map material has the game look")
    if wall is not None:
        surface = look._output(wall.node_tree).inputs["Surface"]
        check(surface.is_linked and surface.links[0].from_node.name == look.SHADER, "the look shader draws it")
    collections = import_map.collision_collections(context, root, create=False)
    check(all(c is not None and c.hide_render for c in collections.values()), "hidden collision is not rendered")
    check(context.scene.world is not None and context.scene.world.name == "c2 t01", "the light set's world")
    check(any(obj.name.startswith("c2 light t01 ") for obj in bpy.data.objects), "the light set's suns")
    check(not any(c2data.map_root(obj) for obj in bpy.data.objects if obj.name.startswith("c2 light ")), "suns outside the map")
    check(look.shown_at([5, 20], 12) and not look.shown_at([5, 20], 22), "a day piece shows at noon, not at night")
    check(look.shown_at([20, 5], 22) and not look.shown_at([20, 5], 12), "a night piece across midnight")
    check(look.shown_at([0, 0], 3) and look.shown_at(None, 3), "pieces without hours always show")
    some_piece = next(obj for obj in c2data.descendants(root) if c2data.role(obj) == "piece")
    c2data.assign(some_piece, ("time",), [20, 5])
    check(look.show_hour(context, 12) == 1, "one piece hidden at noon")
    check(all(obj.hide_render for obj in c2data.descendants(some_piece)), "its nodes hidden from renders")
    c2data.assign(some_piece, ("time",), [0, 0])
    check(look.show_hour(context, 12) == 0, "and shown again")
    some_place = c2data.placement_of(some_piece)
    c2data.assign(some_place, ("show",), 0)
    hidden_now = look.show_hour(context, 12)
    check(hidden_now == len([o for o in some_place.children if c2data.role(o) == "piece"]), "a placement the game hides hides its pieces")
    check(look.show_hour(context, 12, enabled=False) == 0, "every piece shows with the filter off")
    c2data.assign(some_place, ("show",), 1)
    look.show_hour(context, 12)
    for obj in c2data.descendants(some_piece):
        if c2data.is_collision_mesh(obj):
            obj.hide_set(True)

    door = bpy.data.objects["point:exit"]
    check(door.type == "EMPTY" and door.empty_display_type == "CUBE", "door is a cube empty")
    check(abs(door.empty_display_size - 15.0) < 1e-6, "door size 15: %s" % door.empty_display_size)
    photo = bpy.data.objects["point:photo1"]
    check(photo.empty_display_type == "CUBE" and abs(photo.empty_display_size - 40.0) < 1e-6, "invent box size 40")
    lamp = bpy.data.objects["point:lamp0"]
    check(lamp.type == "LIGHT" and lamp.data.type == "POINT", "plight is a point light")
    check(lamp.data.use_custom_distance and abs(lamp.data.cutoff_distance - 300.0) < 1e-4, "light range 300 read back")
    check(abs(lamp.data.energy - 120.0) < 1e-4, "light energy 120 (RAW): %s" % lamp.data.energy)

    expected = {}

    def edited(key, change):
        node = walk(source)[key][0]
        value = json.loads(json.dumps(expected.get(key, node["extras"]["c2"])))
        change(value)
        expected[key] = value

    def activate(obj):
        context.view_layer.objects.active = obj

    activate(door)
    settings.d_event_program = 777
    settings.point_hours = (6, 18)
    settings.d_event_target = "s18"
    bpy.ops.dc2tk.set_event_kind(kind="t_box")
    check(c2data.lookup(door, ("data", "program")) == 777, "event program set")
    check(list(c2data.lookup(door, ("flag",))) == [0, 0, 6, 18], "hours set")
    check(abs(door.empty_display_size - 20.0) < 1e-6, "chest size 20 after the kind changed")
    door_key = (("map:t01", 1), ("points", 1), ("point:exit", 1))

    def change_door(c2):
        c2["data"].update(program=777, target="s18", kind="t_box")
        c2["flag"] = [0, 0, 6, 18]

    edited(door_key, change_door)

    activate(lamp)
    settings.d_plight_color = (128, 128, 255)
    check(abs(lamp.data.color[2] - 1.0) < 1e-6 and abs(lamp.data.color[0] - 128 / 255) < 1e-4, "light colour follows data")
    lamp_key = (("map:t01", 1), ("points", 1), ("point:lamp0", 1))
    edited(lamp_key, lambda c2: c2["data"].update(color=[128, 128, 255]))

    from dc2tk_maps import overlay
    from mathutils import Vector

    lines = overlay.box_lines(photo)

    angle = math.radians(45)
    gx = 50 + math.cos(angle) * 20 + math.sin(angle) * 20
    gy = 40.0
    gz = -50 - math.sin(angle) * 20 + math.cos(angle) * 20
    want = Vector((gx, -gz, gy))
    check(any((Vector(p) - want).length < 1e-3 for p in lines), "invent box corner in the right place")

    house01 = bpy.data.objects["place:house01"]
    house02_obj = bpy.data.objects["place:house02"]
    spin = next(o for o in house01.children if schema.strip_suffix(o.name) == "point:spin")
    bob = bpy.data.objects["point:bob"]
    check(spin.empty_display_type == "ARROWS" and bob.empty_display_type == "ARROWS", "anime points are arrows")
    target, problem = c2data.anime_target(spin)
    check(
        target is not None and schema.strip_suffix(target.name) == "house_body--a40aof-" and c2data.placement_of(target) == house01,
        "a template animation moves the node of its own placement: %s %s" % (target and target.name, problem),
    )
    target, problem = c2data.anime_target(bob)
    check(
        target is not None and schema.strip_suffix(target.name) == "piece:house_m" and target.parent == house02_obj,
        "a map's own animation moves the piece of the placement it names: %s %s" % (target and target.name, problem),
    )
    activate(bob)
    check(settings.d_anime_drives == "POSITION" and settings.d_anime_mode == "PINGPONG", "drives and mode read")
    check(tuple(settings.d_anime_end) == (0.0, 20.0, 0.0) and not settings.d_anime_uniform, "end and uniform read")
    check(schema.anime_vectors(1, 4)["end"][0] == "End (units)", "a position's end is in units")

    lines = overlay.anime_lines(bob)
    check(any((Vector(p) - Vector((-150.0, -220.0, 25.0))).length < 1e-3 for p in lines), "the path's end in the right place: %s" % lines)
    lines = overlay.anime_lines(spin)
    centre = c2data.anime_target(spin)[0].matrix_world.translation
    ends = [Vector(p) - centre for p in lines[:2]]
    check(
        len(lines) > 4 and all(abs(abs(e.z) - overlay.AXIS_HALF) < 1e-3 and e.xy.length < 1e-3 for e in ends),
        "a turn about the game's Y has an upright axis: %s" % ends,
    )
    check(len(overlay.anime_lines(bob, link=True)) == len(overlay.anime_lines(bob)) + 2, "a selected point's line to what it moves")
    settings.d_anime_mode = "REPEAT"
    check(c2data.lookup(bob, ("data", "mode")) == 3, "mode set")
    bob_key = (("map:t01", 1), ("points", 1), ("point:bob", 1))
    edited(bob_key, lambda c2: c2["data"].update(mode=3))
    activate(spin)
    settings.d_anime_uniform = True
    check(c2data.lookup(spin, ("data", "uniform")) == 1, "uniform set")
    spin_key = (("map:t01", 1), ("placements", 1), ("place:house01", 1), ("point:spin", 1))
    edited(spin_key, lambda c2: c2["data"].update(uniform=1))

    house02 = bpy.data.objects["place:house02"]
    activate(house02)
    settings.place_group = "outside"
    settings.place_far_distance = 2500.0
    house02_key = (("map:t01", 1), ("placements", 1), ("place:house02", 1))

    def change_house(c2):
        c2["group"] = "outside"
        c2["far_clip"][0] = 2500.0

    edited(house02_key, change_house)

    bpy.ops.dc2tk.edit_value(object_name=root.name, path=json.dumps(["map", "settings", "light_sets", 0, "fog_enable"]), value="0")
    bpy.ops.dc2tk.edit_value(object_name=root.name, path=json.dumps(["map", "settings", "light_sets", 0, "bgcolor"]), value="[1, 2, 3]")
    root_key = (("map:t01", 1),)

    def change_root(c2):
        c2["map"]["settings"]["light_sets"][0]["fog_enable"] = 0
        c2["map"]["settings"]["light_sets"][0]["bgcolor"] = [1, 2, 3]

    edited(root_key, change_root)
    check(c2data.lookup(root, ("map", "settings", "light_sets", 0, "fog_enable")) == 0, "map setting edited")

    context.scene.cursor.location = (10.0, 20.0, 30.0)
    activate(root)
    for preset in schema.POINT_PRESETS:
        result = bpy.ops.dc2tk.add_point(preset=preset[0], in_placement=False)
        check(result == {"FINISHED"}, "add point %s" % preset[0])
    activate(bpy.data.objects["place:house01"])
    result = bpy.ops.dc2tk.add_point(preset="EVENT", name="talk")
    check(result == {"FINISHED"}, "add point in a placement")
    talk = bpy.data.objects.get("point:talk")
    check(talk is not None and talk.parent and talk.parent.name == "place:house01", "talk is in place:house01")
    body = next(o for o in c2data.descendants(house01) if schema.strip_suffix(o.name) == "house_body--a40aof-")
    activate(body)
    result = bpy.ops.dc2tk.add_point(preset="ANIME", name="wobble")
    check(result == {"FINISHED"}, "add an animation of a model node")
    wobble = bpy.data.objects.get("point:wobble")
    want = {"placement": "house01", "piece": "house_m", "frame": "house_body--a40aof-", "drives": 2, "mode": 1}
    got = {key: c2data.lookup(wobble, ("data", key)) for key in want} if wobble else None
    check(got == want and wobble.parent == house01, "the animation names the active node: %s" % got)
    check(wobble is not None and c2data.anime_target(wobble)[0] == body, "and moves it")
    added_light = bpy.data.objects.get("point:plight")
    check(added_light is not None and added_light.type == "LIGHT", "the added point light is a light")
    added_door = bpy.data.objects.get("point:door")
    check(added_door is not None and added_door.parent.name == "points", "the door went under points")
    if added_door is not None:
        check((added_door.matrix_world.translation - context.scene.cursor.location).length < 1e-4, "added at the cursor")

    activate(bpy.data.objects["place:house01"])
    result = bpy.ops.dc2tk.duplicate_placement()
    check(result == {"FINISHED"}, "duplicate placement")
    copy = bpy.data.objects.get("place:house01_2")
    check(copy is not None, "place:house01_2 made")
    if copy is not None:
        check(c2data.lookup(copy, ("name",)) == "house01_2", "the copy has its own game name")
        check(c2data.lookup(copy, ("slot",)) is None, "the copy has no slot (appended)")
        check(len(c2data.descendants(copy)) == len(c2data.descendants(bpy.data.objects["place:house01"])), "copy has every node")

    for obj in context.selected_objects:
        obj.select_set(False)
    activate(root)
    check(not bpy.ops.dc2tk.new_model.poll(), "new model needs a loose mesh selected")
    bpy.ops.mesh.primitive_cube_add(size=4.0, location=(50.0, 60.0, 5.0))
    crate = context.active_object
    crate.name = "crate"
    result = bpy.ops.dc2tk.new_model()
    check(result == {"FINISHED"}, "new model")
    made = bpy.data.objects.get("place:t01_crate")
    check(made is not None, "place:t01_crate made")
    if made is not None:
        check(c2data.lookup(made, ("template",)) == "t01_crate", "its own template")
        check((made.matrix_world.translation - crate.matrix_world.translation).length < 1e-4, "placed where the mesh is")
        pieces = {c2data.lookup(p, ("model",)): c2data.lookup(p, ("kind",)) for p in made.children}
        check(pieces == {"t01_crate-m.mds": "model", "t01_crate-a.mds": "collision"}, "its pieces: %s" % pieces)
        check(crate.parent is not None and crate.parent.name == "piece:t01_crate-m", "the mesh is under the drawn piece")
        check((crate.matrix_world.translation - Vector((50.0, 60.0, 5.0))).length < 1e-4, "the mesh stayed put")
        walls = [obj for obj in c2data.descendants(made) if obj.type == "MESH" and c2data.is_collision_mesh(obj)]
        check(len(walls) == 1 and all(len(p.vertices) == 3 for p in walls[0].data.polygons), "a triangulated collision copy")
        check(walls and walls[0].data.materials[0].name == "col_ground0_foot0_area6_mask0", "collision area 6")
    crate.select_set(True)
    check(not bpy.ops.dc2tk.new_model.poll(), "the mesh is a model now, not loose")

    bpy.ops.dc2tk.toggle_collision(kind="collision")
    check(floor.visible_get(), "collision shown by the toggle")
    for obj in context.selected_objects:
        obj.select_set(False)
    floor.select_set(True)
    activate(floor)
    bpy.ops.object.mode_set(mode="EDIT")
    import bmesh

    mesh = bmesh.from_edit_mesh(floor.data)
    for face in mesh.faces:
        face.select_set(face.index == 0)
    bmesh.update_edit_mesh(floor.data)
    result = bpy.ops.dc2tk.paint_collision(ground=1, foot=3, area=7, mask=4)
    bpy.ops.object.mode_set(mode="OBJECT")
    check(result == {"FINISHED"}, "paint collision")
    painted = floor.material_slots[floor.data.polygons[0].material_index].material
    check(painted is not None and painted.name == "col_ground1_foot3_area7_mask4", "face 0 painted: %s" % (painted and painted.name))
    bpy.ops.dc2tk.toggle_collision(kind="collision")
    check(not floor.visible_get(), "collision hidden again")

    activate(root)
    bpy.ops.dc2tk.validate()
    errors = [r for r in validate.results if r[0] == validate.ERROR]
    check(not errors, "no validation errors: %s" % errors)
    long_name = bpy.data.objects["point:river"]
    c2data.rename(long_name, "x" * 32)
    quad_check = validate.check(root)
    check(any("bytes" in r[2] for r in quad_check if r[0] == validate.ERROR), "a 32-byte name is an error")
    c2data.rename(long_name, "river")
    check(not validate.left_out(root), "nothing left out of the export: %s" % validate.left_out(root))
    loose = bpy.data.objects.new("loose", bpy.data.meshes.new("loose"))
    context.collection.objects.link(loose)
    check(any(r[1] == "loose" for r in validate.left_out(root)), "a mesh outside the map is reported as left out")
    bpy.data.objects.remove(loose)

    crate_obj = bpy.data.objects.get("crate")
    if crate_obj is not None:
        picture = bpy.data.images.new("crate_wood", 100, 64)
        material = bpy.data.materials.new("crate_wood")
        if bpy.app.version < (5, 0, 0):
            material.use_nodes = True
        material.node_tree.nodes.new("ShaderNodeTexImage").image = picture
        crate_obj.data.materials.append(material)
        found = validate.check(root)
        check(any("crate_wood" in r[2] and "powers of two" in r[2] for r in found if r[0] == validate.ERROR), "a 100x64 picture is an error")
        picture.scale(128, 64)
        found = validate.check(root)
        check(not any("crate_wood" in r[2] for r in found), "a 128x64 picture is fine")
        check(not any("mds_index" in r[2] and r[1] == crate_obj.name for r in found), "no new-node warning on a new model")
    bad = bpy.data.objects.new("point:bad", None)
    context.collection.objects.link(bad)
    bad.parent = c2data.group_node(root, "points")
    bad["c2"] = {"role": "point", "kind": "teleport", "enable": 1, "flag": [0, 0, 0, 0], "data": {}}
    bare = bpy.data.objects.new("Empty", None)
    context.collection.objects.link(bare)
    bare.parent = root
    results = validate.check(root)
    check(any("Unknown point kind" in r[2] for r in results), "unknown kind reported")
    check(any(r[1] == "Empty" and "No extras.c2" in r[2] for r in results), "missing extras reported")

    bad["c2"] = {"role": "point", "kind": "invent", "enable": 1, "flag": [0, 0, 0, 0],
                 "data": {"id": 251, "min": [-1, -1, -1], "max": [1, 1, 1], "unknown_int": 0, "unknown_float": 0, "angle": 0}}
    results = validate.check(root)
    check(any(r[1] == "point:bad" and "never photographed" in r[2] for r in results), "a map's own invent box is reported")
    bpy.data.objects.remove(bad)
    bpy.data.objects.remove(bare)

    import ui_check

    activate(door)
    for obj in (
        root,
        door,
        lamp,
        photo,
        text_door,
        glow,
        spin,
        bob,
        house02,
        bpy.data.objects["piece:house_m"],
        floor,
        bpy.data.objects["camera:0"],
    ):
        activate(obj)
        settings.show_raw = True
        problems = ui_check.draw_all(context)
        check(not problems, "panels draw for %s: %s" % (obj.name, problems))
    settings.show_raw = False

    mod_dir = os.path.join(work, "mods", "testmod")
    os.makedirs(mod_dir)
    with open(os.path.join(mod_dir, "mod.json"), "w") as handle:
        json.dump({"name": "testmod", "replace": {"texture:abc": "a.png"}, "add": {"x": 1}}, handle)
    settings_prefs.mod_dir = mod_dir
    activate(root)
    result = bpy.ops.dc2tk.export_mod()
    check(result == {"FINISHED"}, "export to mod: %s" % result)
    out_path = os.path.join(mod_dir, "maps", "t01", "t01.gltf")
    check(os.path.isfile(out_path), "maps/t01/t01.gltf written")
    with open(os.path.join(mod_dir, "mod.json")) as handle:
        mod = json.load(handle)
    check(mod.get("name") == "testmod" and mod.get("add") == {"x": 1}, "mod.json keeps its other keys")
    check(mod["replace"] == {"texture:abc": "a.png", "map:t01": "maps/t01/t01.gltf"}, "mod.json line: %s" % mod["replace"])

    new_map_dir = os.path.join(work, "mods", "newmap")
    os.makedirs(new_map_dir)
    added = {"map:t01x": {"file": "maps/t01/t01.gltf", "like": "t01"}}
    with open(os.path.join(new_map_dir, "mod.json"), "w") as handle:
        json.dump({"add": added}, handle)
    result = bpy.ops.dc2tk.export_mod(mod_dir=new_map_dir)
    check(result == {"FINISHED"}, "export to a new map's mod: %s" % result)
    with open(os.path.join(new_map_dir, "mod.json")) as handle:
        mod = json.load(handle)
    check(mod == {"add": added}, "a new map's export adds no replace line: %s" % mod)
    check(not floor.visible_get(), "collision still hidden after the export")
    if wall is not None:
        surface = look._output(wall.node_tree).inputs["Surface"]
        check(surface.links[0].from_node.name == look.SHADER, "the look is back after the export")

    with open(out_path, encoding="utf-8") as handle:
        out = json.load(handle)
    before = walk(source)
    after = walk(out)
    for key, (node, world) in before.items():
        if key not in after:
            check(False, "node kept: %s" % key_name(key))
            continue
        new_node, new_world = after[key]
        want = expected.get(key, node.get("extras", {}).get("c2"))
        got = new_node.get("extras", {}).get("c2")
        check(same(want, got), "extras.c2 of %s: %s != %s" % (key_name(key), want, got))
        moved = max(abs(world[i][j] - new_world[i][j]) for i in range(4) for j in range(4))
        check(moved < 1e-3, "%s did not move (%g)" % (key_name(key), moved))
        if "mesh" in node:
            check("mesh" in new_node, "%s keeps its mesh" % key_name(key))
    for node in out["nodes"]:
        name = node.get("name", "")
        check(schema.strip_suffix(name) == name, "no Blender suffix on %s" % name)
        c2 = node.get("extras", {}).get("c2")
        check(isinstance(c2, dict) and c2.get("role") in schema.ROLES, "%s has extras.c2 with a role" % name)
    new_keys = [key for key in after if key not in before]
    new_names = sorted(key_name(key) for key in new_keys)
    print("New nodes:", len(new_names))
    check(len(out["nodes"]) == len(source["nodes"]) + len(new_keys), "no nodes besides the source's and the additions")
    for key in new_keys:
        node = after[key][0]
        c2 = node["extras"]["c2"]
        if c2["role"] == "point":
            check(c2["kind"] in schema.POINT_KINDS, "%s kind" % key_name(key))
            check(isinstance(c2.get("data"), dict) and isinstance(c2.get("flag"), list), "%s data and flag" % key_name(key))
            check(node["name"].startswith("point:"), "%s prefix" % key_name(key))
            if key[1][0] == "points":
                world = after[key][1]

                at = [world[0][3], world[1][3], world[2][3]]
                check(same(at, [10.0, 30.0, -20.0]), "%s at the cursor: %s" % (key_name(key), at))
                rotation = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
                check(same(rotation, [0.0, 0.0, 0.0, 1.0]), "%s not turned: %s" % (key_name(key), rotation))
    check(any(key_name(k) == "map:t01/placements/place:house01_2" for k in new_keys), "the duplicated placement is exported")
    copy_c2 = next((after[k][0]["extras"]["c2"] for k in new_keys if key_name(k).endswith("place:house01_2")), {})
    check("slot" not in copy_c2 and copy_c2.get("template") == "house", "the copy places the same template, no slot")
    check(copy_c2.get("name") == "house01_2", "the exported copy has its own game name")
    check(any(key_name(k) == "map:t01/placements/place:house01/point:talk" for k in new_keys), "the talk point is in house01")

    materials = [m["name"] for m in out.get("materials", [])]
    check("col_ground1_foot3_area7_mask4" in materials, "the painted material is exported: %s" % materials)
    for name in ("col_ground0_foot5_area1_mask12", "col_ground0_foot0_area0_mask0"):
        check(name in materials, "%s exported" % name)

    check("col_ground0_foot2_area6_mask0" not in materials, "the repainted material is gone")
    check(out["scenes"][0].get("name") == "t01", "the scene is named after the map")
    check(
        any(key_name(k) == "map:t01/points/point:point" for k in new_keys),
        "the spawn point is named point in the map's points (names are unique per parent)",
    )
    lights = out.get("extensions", {}).get("KHR_lights_punctual", {}).get("lights", [])
    lamp_node = after[lamp_key][0]
    light = lights[lamp_node["extensions"]["KHR_lights_punctual"]["light"]] if "extensions" in lamp_node else None
    check(light is not None and abs(light.get("range", 0) - 300.0) < 1e-3, "lamp0 keeps its KHR light and range: %s" % light)
    check(light is not None and abs(light.get("intensity", 0) - 120.0) < 1e-3, "lamp0 intensity 120 (RAW)")
    house_key = (("map:t01", 1), ("placements", 1), ("place:house01", 1), ("piece:house_m", 1), ("house_body--a40aof-", 1))
    mesh = out["meshes"][after[house_key][0]["mesh"]]
    check("COLOR_0" in mesh["primitives"][0]["attributes"], "vertex colours exported")
    check(out["asset"].get("generator", "").startswith("Khronos glTF Blender"), "written by Blender's exporter")

    if sys.platform.startswith("win"):
        iso = os.path.join(work, "dc2.iso")
        open(iso, "wb").close()
        settings_prefs.iso_path = iso
        settings_prefs.output_iso = os.path.join(work, "out.iso")
        settings_prefs.source_dir = source_dir
        result = bpy.ops.dc2tk.build()
        check(result == {"FINISHED"}, "dc2tk build ran: %s" % result)
        from dc2tk_maps import runner

        text = "\n".join(runner.output_lines(runner.current)) if runner.current else ""
        check("build" in text and "--mod local.testmod" in text and "--mods" in text, "build arguments: %s" % text)
        check(bpy.data.texts.get(runner.TEXT_NAME) is not None, "output in the text block")
        # an extracted disc folder in place of the image; a folder extract did not write is refused
        folder = os.path.join(work, "extracted_disc")
        os.makedirs(folder)
        settings_prefs.iso_path = folder
        try:
            refused = bpy.ops.dc2tk.build() == {"CANCELLED"}
        except RuntimeError as error:  # the operator's error report
            refused = "Set the retail disc" in str(error)
        check(refused, "a folder without manifest.json is no disc")
        open(os.path.join(folder, "manifest.json"), "w").close()
        result = bpy.ops.dc2tk.build()
        check(result == {"FINISHED"}, "dc2tk build ran from an extracted disc: %s" % result)
        text = "\n".join(runner.output_lines(runner.current)) if runner.current else ""
        check(folder in text, "build reads the extracted disc: %s" % text)
    import test_regressions

    test_regressions.run(context, root, work)
    import test_tools

    test_tools.run(context, root, work, check)
    check_unload(context, source_path)
    return work


def check_unload(context, source_path):
    from dc2tk_maps import c2data, import_map

    # importing again replaces the map in the scene: one root, and as many objects as one import
    result = bpy.ops.dc2tk.import_map_gltf(filepath=source_path)
    check(result == {"FINISHED"}, "the map imports again: %s" % result)
    once = len(bpy.data.objects)
    result = bpy.ops.dc2tk.import_map_gltf(filepath=source_path)
    roots = c2data.scene_map_roots(context.scene)
    check([root.name for root in roots] == ["map:t01"], "one map after importing it again: %s" % roots)
    check(len(bpy.data.objects) == once, "as many objects as one import: %d, %d" % (once, len(bpy.data.objects)))
    # unloading leaves nothing of the map, and keeps what is not the map's
    mine = bpy.data.objects.new("mine", bpy.data.meshes.new("mine"))
    context.scene.collection.objects.link(mine)
    objects = [obj for obj in c2data.descendants(roots[0])]
    meshes = {obj.data.name for obj in objects if obj.type == "MESH"}
    materials = {slot.material.name for obj in objects for slot in obj.material_slots if slot.material}
    collections = {collection.name for obj in objects for collection in obj.users_collection}
    removed = import_map.unload_maps(context)
    check(removed == ["map:t01"], "unload removes the map: %s" % removed)
    check(not c2data.scene_map_roots(context.scene), "no map left in the scene")
    check(not meshes & set(bpy.data.meshes.keys()), "the map's meshes are gone")
    check(not materials & set(bpy.data.materials.keys()), "the map's materials are gone")
    left = collections & set(bpy.data.collections.keys())
    check(not left, "the map's collections are gone: %s" % sorted(left))
    check(mine.name in bpy.data.objects and mine.data.name in bpy.data.meshes, "an object of one's own stays")
    check(import_map.unload_maps(context) == [], "unload with no map does nothing")


if __name__ == "__main__":
    work = None
    try:
        work = main()
    except Exception:
        traceback.print_exc()
        failures.append("exception")
    print("%d checks, %d failures" % (checks, len(failures)))
    if work and not failures and "--keep" not in sys.argv:
        shutil.rmtree(work, ignore_errors=True)
    sys.stdout.flush()
    if failures:
        os._exit(1)
    os._exit(0)
