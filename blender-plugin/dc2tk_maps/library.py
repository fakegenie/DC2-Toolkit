"""Building from the game's own parts: any map's templates placed in the map being edited (their
models and pictures copied in as the map's own, under new names, since a map only reaches its own
packs), and a dungeon layout's rooms laid out from a floor's parts to see them joined."""

import json
import os
import shutil

import bpy
from bpy.props import EnumProperty, IntProperty
from mathutils import Matrix

from . import c2data, edit, import_map, look, prefs, runner, schema

LAYOUT_COLLECTION = "c2 layout"


# --- another map's templates -----------------------------------------------------------------


def export_path(context, name):
    return import_map.find_export(os.path.join(prefs.maps_dir(prefs.get(context)), name), name)


def ensure_export(context, name):
    """The map's exported scene, running dc2tk map-export for it first when there is none."""
    path = export_path(context, name)
    if path is not None:
        return path, None
    settings = prefs.get(context)
    dc2tk = prefs.dc2tk_command(settings)
    if dc2tk is None:
        return None, "dc2tk not found: set the repository (or dc2tk itself) in the add-on's preferences"
    folder = os.path.join(prefs.maps_dir(settings), name)
    os.makedirs(folder, exist_ok=True)
    args = [part.format(name=name, out=folder) for part in settings.export_args.split()]
    code, output = runner.run_quick(dc2tk + args, prefs.working_dir(settings), timeout=600)
    if code != 0:
        return None, "dc2tk map-export %s failed (exit %s)" % (name, code)
    path = export_path(context, name)
    return (path, None) if path else (None, "dc2tk wrote no scene for %s" % name)


def _c2(node):
    value = (node.get("extras") or {}).get("c2")
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except ValueError:
            return {}
    return value if isinstance(value, dict) else {}


def template_nodes(gltf):
    """The first placement node of each template of an exported map: {template: node index}."""
    found = {}
    for index, node in enumerate(gltf.get("nodes", [])):
        data = _c2(node)
        if data.get("role") == "placement":
            found.setdefault(str(data.get("template", "")), index)
    return found


def subset(gltf, top):
    """A glTF holding node top and what is below it, its node and mesh indices made its own (the
    buffers, materials and pictures stay, read only where used)."""
    nodes = gltf["nodes"]
    order, meshes = [], []
    stack = [top]
    while stack:
        index = stack.pop()
        order.append(index)
        stack.extend(reversed(nodes[index].get("children") or []))
    renumber = {old: new for new, old in enumerate(order)}
    out_nodes = []
    for old in order:
        node = dict(nodes[old])
        if node.get("children"):
            node["children"] = [renumber[child] for child in node["children"]]
        if "mesh" in node:
            if node["mesh"] not in meshes:
                meshes.append(node["mesh"])
            node["mesh"] = meshes.index(node["mesh"])
        out_nodes.append(node)
    out = {key: value for key, value in gltf.items() if key not in ("nodes", "meshes", "scenes", "scene")}
    out["nodes"] = out_nodes
    out["meshes"] = [gltf["meshes"][index] for index in meshes]
    out["scenes"] = [{"nodes": [0]}]
    out["scene"] = 0
    return out


def own_name(taken, base, limit=schema.NAME_LIMIT):
    """base, or base_2, base_3... as no name taken is, cut to limit bytes."""
    name = schema.truncate_name(base, limit)
    number = 2
    while name.lower() in taken:
        tail = "_%d" % number
        name = schema.truncate_name(base, limit - len(tail)) + tail
        number += 1
    taken.add(name.lower())
    return name


# the narrowest picture most maps' texture files take (their formats need widths in 16s)
NARROWEST = 16


def widen(image):
    """Scales a picture narrower than most maps take up to that width, keeping its shape (its
    sides stay powers of two), and saves it over its file."""
    image.reload()
    if not image.has_data:
        image.pixels[0]  # loads the file
    width, height = image.size[:]
    if not width or width >= NARROWEST:
        return
    factor = NARROWEST // width
    image.scale(width * factor, height * factor)
    image.save()


def adopt(context, root, placement, source, template):
    """Makes an imported template the map's own: a new placement (no slot) of a new template, its
    pieces' models new models of the map, its pictures new pictures of the map, its collision
    hidden and coloured as the map's."""
    placements = c2data.placements(root)
    taken = {c2data.exact_name(obj, "placement").lower() for obj in placements}
    taken |= {name.lower() for name in c2data.template_names(root)}
    name = own_name(taken, "%s_%s" % (source, template))
    data = c2data.get(placement) or {}
    data.update({"role": "placement", "name": name, "template": name, "show": 1})
    for key in ("slot", "preview", "move_flag"):
        data.pop(key, None)
    c2data.put(placement, data)
    placement.name = schema.PREFIX["placement"] + name
    models = {str(c2data.lookup(obj, ("model",), "")).lower() for obj in c2data.descendants(root) if c2data.role(obj) == "piece"}
    renamed = {}
    children = c2data.child_map()
    for piece in [obj for obj in c2data.descendants(placement, children) if c2data.role(obj) == "piece"]:
        model = str(c2data.lookup(piece, ("model",), ""))
        if model not in renamed:
            stem = model[:-4] if model.lower().endswith(".mds") else model
            renamed[model] = own_name(models, "%s_%s" % (source, stem), schema.NAME_LIMIT - len(".mds")) + ".mds"
        c2data.assign(piece, ("model",), renamed[model])
        for node in c2data.descendants(piece, children)[1:]:
            if c2data.role(node) == "mds":
                c2data.make_new_node(node)
    pictures = set()
    for image in bpy.data.images:
        pictures.add(schema.strip_suffix(image.name).lower())
    from . import validate

    seen, images = set(), {}
    for obj in c2data.descendants(placement):
        if obj.type != "MESH" or c2data.is_collision_mesh(obj):
            continue
        for slot in obj.material_slots:
            material = slot.material
            if material is None or material.name in seen:
                continue
            seen.add(material.name)
            for node in material.node_tree.nodes if material.node_tree else []:
                if node.type != "TEX_IMAGE" or node.image is None:
                    continue
                if node.image.name not in images:
                    new = own_name(pictures, "%s_%s" % (source, validate.picture_name(node.image)))
                    # the exporter names a picture it has not changed after its file: a copy under
                    # the new name makes it the map's own
                    path = bpy.path.abspath(node.image.filepath or "")
                    if path and os.path.isfile(path):
                        copy = os.path.join(os.path.dirname(path), new + ".png")
                        shutil.copyfile(path, copy)
                        # the importer packs pictures into the .blend, and the exporter writes
                        # what is packed: the copy is the picture now
                        if node.image.packed_file is not None:
                            node.image.unpack(method="REMOVE")
                        node.image.filepath = copy
                        widen(node.image)
                    node.image.name = new
                    images[node.image.name] = new
                if material.get("texture") is not None:
                    material["texture"] = images[node.image.name]
    collections = import_map.collision_collections(context, root)
    children = c2data.child_map()
    for piece in [obj for obj in c2data.descendants(placement, children) if c2data.role(obj) == "piece"]:
        kind = c2data.lookup(piece, ("kind",))
        if kind in schema.COLLISION_KINDS:
            for obj in c2data.descendants(piece, children):
                import_map.move_to(obj, collections[kind])
    import_map.colour_collision(root, context.window_manager.dc2tk_maps.collision_colour)
    materials = {slot.material for obj in c2data.descendants(placement) if obj.type == "MESH" for slot in obj.material_slots}
    look.apply_all([material for material in materials if material is not None])
    for obj in [obj for obj in c2data.descendants(placement) if c2data.role(obj) == "point"]:
        if c2data.lookup(obj, ("kind",)) == "plight":
            import_map.ensure_light(obj)
        else:
            import_map.point_display(obj)
    return name, sorted(renamed.values())


def load_templates(context, name):
    path, problem = ensure_export(context, name)
    if path is None:
        return problem
    settings = context.window_manager.dc2tk_maps
    entries = settings.library_templates
    entries.clear()
    for template in sorted(template_nodes(import_map.read_gltf(path))):
        entry = entries.add()
        entry.code = template
        entry.name = template
    return None


class DC2TK_OT_load_library(bpy.types.Operator):
    """List the templates of a map (exported with dc2tk map-export when it has not been), to
    place one in the map being edited"""

    bl_idname = "dc2tk.load_library"
    bl_label = "List Templates"

    def execute(self, context):
        settings = context.window_manager.dc2tk_maps
        name = import_map.map_code(settings.library_map)
        if not name:
            self.report({"ERROR"}, "Pick the map to take templates from")
            return {"CANCELLED"}
        problem = load_templates(context, name)
        if problem:
            self.report({"ERROR"}, problem)
            return {"CANCELLED"}
        self.report({"INFO"}, "%s: %d templates" % (name, len(settings.library_templates)))
        return {"FINISHED"}


class DC2TK_OT_add_template(bpy.types.Operator):
    """Place a template of another map at the 3D cursor: a new placement of a template of its own,
    whose models and pictures the build adds to this map under new names (a map reaches only its
    own packs)"""

    bl_idname = "dc2tk.add_template"
    bl_label = "Template from a Map"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return c2data.current_map_root(context) is not None

    def invoke(self, context, event):
        settings = context.window_manager.dc2tk_maps
        if not settings.library_template:
            return context.window_manager.invoke_props_dialog(self, width=360)
        return self.execute(context)

    def draw(self, context):
        settings = context.window_manager.dc2tk_maps
        row = self.layout.row(align=True)
        row.prop_search(settings, "library_map", settings, "maps", text="Map")
        row.operator(DC2TK_OT_load_library.bl_idname, text="", icon="FILE_REFRESH")
        self.layout.prop_search(settings, "library_template", settings, "library_templates", text="Template")

    def execute(self, context):
        settings = context.window_manager.dc2tk_maps
        root = c2data.current_map_root(context)
        source = import_map.map_code(settings.library_map)
        template = settings.library_template
        if not source or not template:
            self.report({"ERROR"}, "Pick a map and one of its templates (List Templates)")
            return {"CANCELLED"}
        path, problem = ensure_export(context, source)
        if path is None:
            self.report({"ERROR"}, problem)
            return {"CANCELLED"}
        gltf = import_map.read_gltf(path)
        top = template_nodes(gltf).get(template)
        if top is None:
            self.report({"ERROR"}, "%s has no template %s" % (source, template))
            return {"CANCELLED"}
        part = os.path.join(os.path.dirname(path), "_template_%s.gltf" % template)
        with open(part, "w", encoding="utf-8") as handle:
            json.dump(subset(gltf, top), handle)
        before = set(bpy.data.objects)
        try:
            bpy.ops.import_scene.gltf(**import_map.operator_kwargs(bpy.ops.import_scene.gltf, {
                "filepath": part, "merge_vertices": False, "import_shading": "NORMALS",
                "export_import_convert_lighting_mode": "RAW", "guess_original_bind_pose": False,
            }))
        except RuntimeError as error:
            self.report({"ERROR"}, "Blender's glTF importer failed: %s" % str(error).strip().splitlines()[-1])
            return {"CANCELLED"}
        finally:
            os.remove(part)
        made = [obj for obj in bpy.data.objects if obj not in before]
        placement = next((obj for obj in made if obj.parent is None and c2data.role(obj) == "placement"), None)
        if placement is None:
            bpy.data.batch_remove(made)
            self.report({"ERROR"}, "The template came in without its placement")
            return {"CANCELLED"}
        group = edit.ensure_group(root, "placements", context)
        for obj in made:
            for collection in list(obj.users_collection):
                collection.objects.unlink(obj)
            edit.link_like(obj, group, context)
        world = placement.matrix_world.copy()
        world.translation = context.scene.cursor.location
        edit.place_at(placement, group, world)
        name, models = adopt(context, root, placement, source, template)
        for other in context.selected_objects:
            other.select_set(False)
        placement.select_set(True)
        context.view_layer.objects.active = placement
        self.report({"INFO"}, "Placed %s from %s: %d new models" % (name, source, len(models)))
        return {"FINISHED"}


# --- a dungeon layout ------------------------------------------------------------------------


def layouts(context):
    root = prefs.repo(prefs.get(context))
    path = os.path.join(root, "extracted", "data", "layouts.json") if root else ""
    try:
        with open(path, encoding="utf-8") as handle:
            return json.load(handle).get("layouts") or []
    except (OSError, ValueError, AttributeError):
        return []


_layout_items = []


def layout_choices(self, context):
    _layout_items[:] = [(entry.get("layout", "?"), entry.get("layout", "?"), "") for entry in layouts(context)]
    return _layout_items or [("", "(no extracted/data/layouts.json)", "")]


def cells(room):
    """A room's cells: (column, row, part name) for each that names a part."""
    found = []
    for row, line in enumerate(room.get("cells") or []):
        for column, cell in enumerate(line.split()):
            part = cell.split(":", 1)[0]
            if part and part != "." and not part.startswith("#"):
                found.append((column, row, part))
    return found


def clear_layout():
    collection = bpy.data.collections.get(LAYOUT_COLLECTION)
    if collection is None:
        return
    bpy.data.batch_remove(list(collection.objects))
    bpy.data.collections.remove(collection)


class DC2TK_OT_layout_preview(bpy.types.Operator):
    """Lay out a dungeon layout's room from the floor's templates, cell by cell (x 320, 0, y 320),
    to see its parts joined: copies outside the map, never exported (run again with no room to
    clear)"""

    bl_idname = "dc2tk.layout_preview"
    bl_label = "Preview Dungeon Layout"
    bl_options = {"REGISTER", "UNDO"}

    layout_name: EnumProperty(name="Layout", items=layout_choices)
    room: IntProperty(name="Room", default=0, min=-1, description="-1 clears the preview")

    @classmethod
    def poll(cls, context):
        root = c2data.current_map_root(context)
        return root is not None and any(c2data.is_preview(obj) for obj in c2data.placements(root))

    def invoke(self, context, event):
        name = c2data.map_name(c2data.current_map_root(context))
        if any(item[0] == name for item in layout_choices(self, context)):
            self.layout_name = name
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        clear_layout()
        if self.room < 0:
            return {"FINISHED"}
        root = c2data.current_map_root(context)
        entry = next((entry for entry in layouts(context) if entry.get("layout") == self.layout_name), None)
        if entry is None or self.room >= len(entry.get("rooms") or []):
            self.report({"ERROR"}, "No room %d in layout %s" % (self.room, self.layout_name))
            return {"CANCELLED"}
        grid = entry.get("grid_size") or [schema.DUNGEON_CELL, schema.DUNGEON_CELL]
        previews = {c2data.template_name(obj): obj for obj in c2data.placements(root) if c2data.is_preview(obj)}
        collection = bpy.data.collections.new(LAYOUT_COLLECTION)
        context.scene.collection.children.link(collection)
        missing = set()
        placed = 0
        origin = root.matrix_world
        for column, row, part in cells(entry["rooms"][self.room]):
            source = previews.get(part)
            if source is None:
                missing.add(part)
                continue
            for obj in c2data.descendants(source):
                if obj.type != "MESH" or c2data.is_collision_mesh(obj):
                    continue
                copy = obj.copy()
                for key in list(copy.keys()):
                    del copy[key]
                copy.parent = None
                within = source.matrix_world.inverted() @ obj.matrix_world
                cell = Matrix.Translation(schema.game_to_blender((column * float(grid[0]), 0.0, row * float(grid[1]))))
                copy.matrix_world = origin @ cell @ within
                collection.objects.link(copy)
            placed += 1
        if missing:
            self.report({"WARNING"}, "The map has no template %s" % ", ".join(sorted(missing)))
        self.report({"INFO"}, "Laid out %d cells of %s room %d" % (placed, self.layout_name, self.room))
        return {"FINISHED"}


classes = (DC2TK_OT_load_library, DC2TK_OT_add_template, DC2TK_OT_layout_preview)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
