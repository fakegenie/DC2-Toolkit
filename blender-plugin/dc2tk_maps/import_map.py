import glob
import json
import os

import bpy
from bpy.props import BoolProperty, StringProperty
from bpy_extras.io_utils import ImportHelper

from . import c2data, look, prefs, runner, schema


def operator_kwargs(operator, wanted):
    names = {prop.identifier for prop in operator.get_rna_type().properties}
    return {key: value for key, value in wanted.items() if key in names}


def read_gltf(path):
    if path.lower().endswith(".glb"):
        import struct

        with open(path, "rb") as handle:
            head = handle.read(20)
            length = struct.unpack_from("<I", head, 12)[0]
            return json.loads(handle.read(length).decode("utf-8"))
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def match_nodes(gltf, roots):
    nodes = gltf.get("nodes", [])
    scene = gltf.get("scenes", [{}])[gltf.get("scene", 0)] if gltf.get("scenes") else {}
    found = {}
    children = c2data.child_map()

    def pair(indices, objects):
        by_name = {}
        for obj in sorted(objects, key=lambda o: (len(o.name), o.name)):
            by_name.setdefault(schema.strip_suffix(obj.name), []).append(obj)
        for index in indices:
            name = nodes[index].get("name", "")
            candidates = by_name.get(name) or by_name.get(schema.strip_suffix(name))
            if not candidates:
                continue
            obj = candidates.pop(0)
            found[index] = obj
            pair(nodes[index].get("children") or [], children.get(obj, []))

    pair(scene.get("nodes", []), roots)
    return found


def ensure_collection(parent, name):
    collection = None
    for child in parent.children:
        if schema.strip_suffix(child.name) == name:
            collection = child
            break
    if collection is None:
        collection = bpy.data.collections.new(name)
        parent.children.link(collection)
    return collection


def find_layer_collection(layer, collection):
    if layer.collection == collection:
        return layer
    for child in layer.children:
        found = find_layer_collection(child, collection)
        if found is not None:
            return found
    return None


def collision_collections(context, root, create=True):
    home = root.users_collection[0] if root.users_collection else context.scene.collection
    name = c2data.map_name(root)
    found = {}
    for kind, label in (("collision", "collision"), ("camera", "camera collision")):
        title = "%s %s" % (name, label)
        if create:
            found[kind] = ensure_collection(home, title)
        else:
            found[kind] = next((c for c in home.children if schema.strip_suffix(c.name) == title), None)
    return found


def set_collision_visible(context, root, kind, visible):
    collection = collision_collections(context, root, create=False).get(kind)
    if collection is None:
        return False
    layer = find_layer_collection(context.view_layer.layer_collection, collection)
    if layer is not None:
        layer.hide_viewport = not visible

    collection.hide_render = not visible
    return True


def collision_visible(context, root, kind):
    collection = collision_collections(context, root, create=False).get(kind)
    if collection is None:
        return None
    layer = find_layer_collection(context.view_layer.layer_collection, collection)
    return layer is not None and not layer.hide_viewport


def move_to(obj, collection):
    if collection in obj.users_collection and len(obj.users_collection) == 1:
        return
    if obj.name not in collection.objects:
        collection.objects.link(obj)
    for other in list(obj.users_collection):
        if other != collection:
            other.objects.unlink(obj)


def colour_collision(root, mode):
    done = set()
    for obj in c2data.collision_meshes(root):
        camera = c2data.lookup(c2data.piece_of(obj), ("kind",)) == "camera"
        for slot in obj.material_slots:
            material = slot.material
            if material is None or material.name in done:
                continue
            done.add(material.name)
            attrs = schema.parse_col(material.name)
            if attrs is None:
                material.diffuse_color = (1.0, 0.0, 1.0, 1.0)
            elif camera:
                material.diffuse_color = (0.55, 0.35, 0.85, 0.6)
            else:
                material.diffuse_color = schema.collision_colour(attrs, mode)
        obj.display_type = "SOLID"
        obj.show_wire = True


def point_display(obj):
    if obj.type != "EMPTY":
        return
    kind = c2data.lookup(obj, ("kind",))
    if kind == "event":
        event_kind = c2data.lookup(obj, ("data", "kind"))
        obj.empty_display_type = "CUBE"
        scale = obj.scale
        near_one = all(abs(abs(s) - 1.0) < 0.01 for s in scale)
        obj.empty_display_size = schema.EVENT_DEFAULT_SIZE.get(event_kind, 1.0) if near_one else 1.0
    elif kind == "invent":
        field_min, field_max = schema.DATA_FIELDS["invent"][1], schema.DATA_FIELDS["invent"][2]
        low = c2data.data_field(obj, field_min)
        high = c2data.data_field(obj, field_max)
        extent = max([abs(float(v)) for v in list(low) + list(high)] + [1.0])
        obj.empty_display_type = "CUBE"
        obj.empty_display_size = extent
    elif kind == "sound":
        near = c2data.data_field(obj, schema.DATA_FIELDS["sound"][1])
        obj.empty_display_type = "SPHERE"
        obj.empty_display_size = max(float(near or 0.0), 5.0)
    elif kind == "pos":
        obj.empty_display_type = "SINGLE_ARROW"
        obj.empty_display_size = 15.0
    elif kind == "anime":
        obj.empty_display_type = "ARROWS"
        obj.empty_display_size = 10.0
    else:
        obj.empty_display_type = "PLAIN_AXES"
        obj.empty_display_size = 10.0
    obj.show_name = False


def light_values(obj):
    rgb = c2data.data_field(obj, schema.DATA_FIELDS["plight"][1])
    power = c2data.data_field(obj, schema.DATA_FIELDS["plight"][0])
    colour = [max(0.0, min(1.0, float(c) / 255.0)) for c in rgb]
    return colour, float(power or 0.0)


def light_correction():
    import math

    from mathutils import Matrix

    return Matrix.Rotation(math.radians(90.0), 4, "X")


def ensure_light(obj):
    if obj.type == "LIGHT":
        return obj
    if obj.type != "EMPTY":
        return obj
    name = obj.name
    colour, power = light_values(obj)
    light = bpy.data.lights.new(schema.game_name(name, "point"), "POINT")
    light.color = colour
    light.energy = power
    obj.name = name + "~old"
    new = bpy.data.objects.new(name, light)
    for collection in obj.users_collection:
        collection.objects.link(new)
    new.parent = obj.parent
    new.matrix_parent_inverse = obj.matrix_parent_inverse.copy()
    new.rotation_mode = obj.rotation_mode
    new.matrix_basis = obj.matrix_basis @ light_correction()
    if c2data.KEY in obj:
        new[c2data.KEY] = obj[c2data.KEY]
    for child in list(obj.children):
        world = child.matrix_world.copy()
        child.parent = new
        child.matrix_world = world
    bpy.data.objects.remove(obj)
    return new


def postprocess(context, gltf_path, new_objects, log=None):
    warnings = []
    roots = [obj for obj in new_objects if obj.parent is None and c2data.role(obj) == "map"]
    if not roots:
        roots = [obj for obj in new_objects if obj.parent is None and obj.name.startswith("map:")]
    if not roots:
        warnings.append("No map: root node (role \"map\") in the scene")
        return roots, warnings

    try:
        gltf = read_gltf(gltf_path) if gltf_path else None
    except (OSError, ValueError) as error:
        gltf = None
        warnings.append("Could not read %s: %s" % (gltf_path, error))
    matched = match_nodes(gltf, [obj for obj in new_objects if obj.parent is None]) if gltf else {}

    matched = {index: obj.name for index, obj in matched.items()}

    for obj in new_objects:
        if c2data.is_stringified(obj):
            warnings.append(
                "%s: Blender could not store extras.c2 as properties (kept as text; "
                "the export writes it back as it was)" % obj.name
            )

    mode = context.window_manager.dc2tk_maps.collision_colour if hasattr(context.window_manager, "dc2tk_maps") else "AREA"
    children = c2data.child_map()
    for root in roots:
        collections = collision_collections(context, root)
        for piece in c2data.collision_pieces(root):
            kind = c2data.lookup(piece, ("kind",))
            for obj in c2data.descendants(piece, children):
                move_to(obj, collections[kind])
        for kind in collections:
            set_collision_visible(context, root, kind, False)
        colour_collision(root, mode)

        for obj in c2data.descendants(root, children):
            role = c2data.role(obj)
            if role == "placement" and obj.type == "EMPTY":
                obj.empty_display_type = "PLAIN_AXES"
                obj.empty_display_size = 20.0
            elif role == "group" and obj.type == "EMPTY":
                obj.empty_display_size = 5.0

        materials = {slot.material for obj in c2data.descendants(root, children) if obj.type == "MESH" for slot in obj.material_slots}
        look.apply_all([material for material in materials if material is not None])
        if hasattr(context.window_manager, "dc2tk_maps"):
            settings = context.window_manager.dc2tk_maps
            look.map_world(context, root, look.lighting_at(root, settings.hour if settings.by_hour else None))
            look.show_hour(context, settings.hour, settings.by_hour, [root])
        else:
            look.map_world(context, root)
            look.show_hour(context, 12, True, [root])

        for obj in c2data.points(root):
            if c2data.lookup(obj, ("kind",)) == "plight":
                ensure_light(obj)
            else:
                point_display(obj)

    if gltf:
        lights = gltf.get("extensions", {}).get("KHR_lights_punctual", {}).get("lights", [])
        for index, name in matched.items():
            node = gltf["nodes"][index]
            light_index = node.get("extensions", {}).get("KHR_lights_punctual", {}).get("light")
            if light_index is None or light_index >= len(lights):
                continue
            obj = bpy.data.objects.get(name)
            if obj is not None and obj.type == "LIGHT" and "range" in lights[light_index]:
                obj.data.use_custom_distance = True
                obj.data.cutoff_distance = float(lights[light_index]["range"])

    for screen in getattr(context, "screen", None) and [context.screen] or []:
        for area in screen.areas:
            if area.type == "VIEW_3D":
                for space in area.spaces:
                    if space.type == "VIEW_3D":
                        space.clip_start = 1.0
                        space.clip_end = max(space.clip_end, 100000.0)

                        shading = space.shading
                        if shading.type in ("SOLID", "WIREFRAME"):
                            shading.type = "MATERIAL"
                        shading.use_scene_world = True
                        shading.use_scene_lights = True
                region = next((region for region in area.regions if region.type == "WINDOW"), None)
                if region is not None and roots:
                    frame_map(context, area, region, roots)

    for obj in context.selected_objects:
        obj.select_set(False)
    if roots:
        context.view_layer.objects.active = roots[0]
    return roots, warnings


def frame_map(context, area, region, roots):
    import math

    from mathutils import Euler, Vector

    spots = [obj.matrix_world.translation.copy() for root in roots for obj in c2data.points(root)]
    if len(spots) < 2:
        spots = [
            obj.matrix_world @ (sum((Vector(corner) for corner in obj.bound_box), Vector()) / 8.0)
            for root in roots
            for obj in c2data.descendants(root)
            if obj.type == "MESH" and not c2data.is_collision_mesh(obj)
        ]
    if not spots:
        return
    low, high = [], []
    for axis in range(3):
        values = sorted(spot[axis] for spot in spots)
        cut = len(values) // 10
        low.append(values[cut])
        high.append(values[len(values) - 1 - cut])
    low, high = Vector(low), Vector(high)
    view = area.spaces.active.region_3d
    view.view_location = (low + high) / 2.0
    view.view_distance = max((high - low).length, 300.0) * 1.1
    view.view_rotation = Euler((math.radians(55.0), 0.0, math.radians(30.0))).to_quaternion()
    view.view_perspective = "PERSP"


def import_gltf(context, path):
    before = set(bpy.data.objects)
    if context.mode != "OBJECT" and context.active_object is not None:
        bpy.ops.object.mode_set(mode="OBJECT")
    wanted = {
        "filepath": path,
        "merge_vertices": False,
        "import_shading": "NORMALS",
        "export_import_convert_lighting_mode": "RAW",
        "guess_original_bind_pose": False,
        "import_select_created_objects": True,
        "import_scene_extras": True,
        "import_scene_as_collection": True,
        "import_merge_material_slots": False,
    }
    try:
        result = bpy.ops.import_scene.gltf(**operator_kwargs(bpy.ops.import_scene.gltf, wanted))
    except RuntimeError as error:
        return None, ["Blender's glTF importer failed on %s: %s" % (path, str(error).strip().splitlines()[-1])]
    if "FINISHED" not in result:
        return None, ["Blender's glTF importer failed on %s" % path]
    new_objects = [obj for obj in bpy.data.objects if obj not in before]
    return postprocess(context, path, new_objects)


def map_code(text):
    """The map's own name in what the map field holds: "m01  Palm Brinks" from the list, or
    "m01" typed."""
    words = text.split()
    return words[0] if words else ""


def load_map_list(context):
    """Fills the map list from dc2tk map-export --list; returns what went wrong, or None."""
    settings = prefs.get(context)
    dc2tk = prefs.dc2tk_command(settings)
    if dc2tk is None:
        return "dc2tk not found: set the repository (or dc2tk itself) in the add-on's preferences"
    code, output = runner.run_quick(dc2tk + ["map-export", "--list"], prefs.working_dir(settings))
    if code != 0:
        last = output.strip().splitlines()[-1:] or ["no output"]
        return "dc2tk map-export --list failed (exit %s): %s" % (code, last[0])
    entries = context.window_manager.dc2tk_maps.maps
    entries.clear()
    for line in output.splitlines():
        name, _, title = line.partition("\t")
        if not name.strip():
            continue
        entry = entries.add()
        entry.code = name.strip()
        entry.title = title.strip()
        entry.name = "%s  %s" % (entry.code, entry.title) if entry.title else entry.code
    return None


# The kinds of data a map's objects use that unloading removes with them, by ID.id_type, each
# with its bpy.data collection; users come before what they use.
USED_DATA = {
    "MESH": "meshes",
    "CURVE": "curves",
    "LIGHT": "lights",
    "CAMERA": "cameras",
    "ARMATURE": "armatures",
    "ACTION": "actions",
    "WORLD": "worlds",
    "MATERIAL": "materials",
    "IMAGE": "images",
}


def unload_maps(context):
    """Removes every map from the scene before another is imported: each map's objects, the
    collections the import made for it (the scene's and its collision collections) once they hold
    nothing else, and the meshes, materials, images, actions and world only it used. Other objects
    stay. Returns the names of the maps removed."""
    roots = c2data.scene_map_roots(context.scene)
    if not roots:
        return []
    if context.mode != "OBJECT" and context.active_object is not None:
        bpy.ops.object.mode_set(mode="OBJECT")
    from . import animate, library

    # what was played goes back, and a dungeon layout laid out of the map goes with it
    animate.stop()
    library.clear_layout()
    names = [root.name for root in roots]
    objects = {obj for root in roots for obj in c2data.descendants(root)}
    # the map's collection (when the scene was imported as one) and its collision collections,
    # which sit in that collection or, beside the root, in the scene's
    homes = {
        collection.name
        for root in roots
        for collection in root.users_collection
        if collection != context.scene.collection
    }
    for root in roots:
        homes.update(found.name for found in collision_collections(context, root, create=False).values() if found)
    # the data the maps use, by name, since removing one block can free others: (bpy.data's
    # collection, name), looked up again before each removal
    used = set()

    def note(block):
        collection = USED_DATA.get(getattr(block, "id_type", None))
        if collection is not None:
            used.add((collection, block.name))

    for obj in objects:
        if obj.data is not None:
            note(obj.data)
        if obj.animation_data is not None and obj.animation_data.action is not None:
            note(obj.animation_data.action)
        for slot in getattr(obj, "material_slots", []):
            if slot.material is not None:
                note(slot.material)
                if slot.material.node_tree is not None:
                    for node in slot.material.node_tree.nodes:
                        if node.type == "TEX_IMAGE" and node.image is not None:
                            note(node.image)
    for root in roots:
        world = bpy.data.worlds.get("c2 %s" % c2data.map_name(root))
        if world is not None:
            if context.scene.world == world:
                context.scene.world = None
            note(world)
    bpy.data.batch_remove(list(objects))
    # what the maps used, once nothing else does: in USED_DATA's order, so a material goes
    # before the images it shows
    for collection in dict.fromkeys(USED_DATA.values()):
        blocks = getattr(bpy.data, collection)
        for kind, name in sorted(used):
            block = blocks.get(name) if kind == collection else None
            if block is not None and block.users == 0:
                blocks.remove(block)

    def remove_empty(collection):
        for child in list(collection.children):
            remove_empty(child)
        if not collection.objects and not collection.children:
            bpy.data.collections.remove(collection)

    for name in sorted(homes):
        collection = bpy.data.collections.get(name)
        if collection is not None:
            remove_empty(collection)
    return names


def find_export(folder, name):
    candidates = [
        os.path.join(folder, name + ".gltf"),
        os.path.join(folder, name, name + ".gltf"),
        os.path.join(folder, name + ".glb"),
    ]
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    found = sorted(
        glob.glob(os.path.join(folder, "**", "*.gltf"), recursive=True),
        key=os.path.getmtime,
        reverse=True,
    )
    return found[0] if found else None


def report_result(operator, roots, warnings):
    for warning in warnings[:20]:
        operator.report({"WARNING"}, warning)
    if len(warnings) > 20:
        operator.report({"WARNING"}, "... and %d more warnings" % (len(warnings) - 20))
    if roots:
        operator.report({"INFO"}, "Imported %s" % ", ".join(root.name for root in roots))
        return {"FINISHED"}
    return {"CANCELLED"}


class DC2TK_OT_import_gltf(bpy.types.Operator, ImportHelper):
    """Import a map scene that dc2tk map-export wrote (keeps every extras.c2, hides collision)"""

    bl_idname = "dc2tk.import_map_gltf"
    bl_label = "Import DC2 Map (.gltf)"
    bl_options = {"REGISTER", "UNDO"}

    filename_ext = ".gltf"
    filter_glob: StringProperty(default="*.gltf;*.glb", options={"HIDDEN"})
    unload: BoolProperty(
        name="Unload other maps",
        description="Remove the maps already in the scene (and the data only they use) first",
        default=True,
    )

    def execute(self, context):
        if self.unload:
            unload_maps(context)
        roots, warnings = import_gltf(context, self.filepath)
        if roots is None:
            for warning in warnings:
                self.report({"ERROR"}, warning)
            return {"CANCELLED"}
        return report_result(self, roots, warnings)


class DC2TK_OT_import_map(bpy.types.Operator):
    """Export a map with dc2tk map-export, then import it"""

    bl_idname = "dc2tk.import_map"
    bl_label = "Import DC2 Map (dc2tk)"
    bl_options = {"REGISTER", "UNDO"}

    map_name: StringProperty(name="Map", description="The map to export and import (s17, d01f01, ...)")
    unload: BoolProperty(
        name="Unload other maps",
        description="Remove the maps already in the scene (and the data only they use) first",
        default=True,
    )
    out_dir: StringProperty(
        name="Folder",
        description="Where dc2tk writes the export; empty for <exported maps>/NAME",
        subtype="DIR_PATH",
    )

    def folder(self, context):
        if self.out_dir:
            return prefs.absolute(self.out_dir)
        base = prefs.maps_dir(prefs.get(context))
        return os.path.join(base, self.map_name) if base else ""

    def execute(self, context):
        return runner.execute(self, context)

    def modal(self, context, event):
        return runner.modal(self, context, event)

    def command(self, context):
        if not self.map_name:
            self.map_name = context.window_manager.dc2tk_maps.import_name
        name = map_code(self.map_name)
        self.map_name = name
        if not name:
            self.report({"ERROR"}, "Give the map's name (s17, d01f01, ...)")
            return None
        settings = prefs.get(context)
        dc2tk = prefs.dc2tk_command(settings)
        if dc2tk is None:
            self.report({"ERROR"}, "dc2tk not found: set the repository (or dc2tk itself) in the add-on's preferences")
            return None
        folder = self.folder(context)
        if not folder:
            self.report({"ERROR"}, "No folder to export to: set the repository or the exported maps folder")
            return None
        os.makedirs(folder, exist_ok=True)

        args = [part.format(name=name, out=folder) for part in settings.export_args.split()]
        return ("dc2tk map-export", dc2tk + args, prefs.working_dir(settings))

    def finish(self, context, job):
        if job["returncode"] != 0:
            self.report({"ERROR"}, "dc2tk map-export failed (exit %s); see the text \"%s\"" % (job["returncode"], runner.TEXT_NAME))
            return {"CANCELLED"}
        path = find_export(self.folder(context), self.map_name)
        if path is None:
            self.report({"ERROR"}, "dc2tk wrote no .gltf into %s" % self.folder(context))
            return {"CANCELLED"}
        if self.unload:
            unload_maps(context)
        roots, warnings = import_gltf(context, path)
        if roots is None:
            for warning in warnings:
                self.report({"ERROR"}, warning)
            return {"CANCELLED"}
        return report_result(self, roots, warnings)

    def invoke(self, context, event):
        if not self.map_name and not context.window_manager.dc2tk_maps.import_name:
            return context.window_manager.invoke_props_dialog(self)
        return runner.invoke(self, context)


class DC2TK_OT_load_maps(bpy.types.Operator):
    """Read the disc's maps for the map list again (dc2tk map-export --list)"""

    bl_idname = "dc2tk.load_maps"
    bl_label = "Reload Map List"

    def execute(self, context):
        problem = load_map_list(context)
        if problem:
            self.report({"ERROR"}, problem)
            return {"CANCELLED"}
        self.report({"INFO"}, "%d maps" % len(context.window_manager.dc2tk_maps.maps))
        return {"FINISHED"}


_list_tried = False


def load_map_list_once():
    """Fills the map list the first time the panel is drawn without one: a panel cannot change
    data while it draws, so the load runs from a timer. A failure is left for Reload to report."""
    global _list_tried
    if _list_tried:
        return
    _list_tried = True

    def load():
        context = bpy.context
        if load_map_list(context) is None:
            for window in context.window_manager.windows:
                for area in window.screen.areas:
                    if area.type == "VIEW_3D":
                        area.tag_redraw()
        return None

    bpy.app.timers.register(load, first_interval=0.0)


def menu_import(self, context):
    self.layout.operator(DC2TK_OT_import_gltf.bl_idname, text="DC2 map (.gltf)")
    self.layout.operator(DC2TK_OT_import_map.bl_idname, text="DC2 map (dc2tk map-export)")


classes = (DC2TK_OT_import_gltf, DC2TK_OT_import_map, DC2TK_OT_load_maps)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_import.append(menu_import)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_import)
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
