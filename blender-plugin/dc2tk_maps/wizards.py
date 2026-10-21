"""Wizards for what takes several files: a door into another map (the point, the program it runs
and the line of mod.json that gives it the program), a map of the mod's own (made of the map in
the scene), and naming an event point's program."""

import os
import re
from collections import OrderedDict

import bpy
from bpy.props import BoolProperty, EnumProperty, StringProperty

from . import c2data, edit, export_mod, import_map, prefs, schema, scripts, tools


def constant(text):
    return re.sub(r"[^A-Z0-9_]", "_", text.upper()) or "DOOR"


def mod_dir_of(operator, context):
    mod_dir = prefs.absolute(prefs.get(context).mod_dir).rstrip("\\/")
    if not mod_dir:
        operator.report({"ERROR"}, "Choose the mod folder (mods/NAME) in Check, Export, Build")
        return None
    os.makedirs(mod_dir, exist_ok=True)
    return mod_dir


# --- a door into another map ------------------------------------------------------------------

DOORS_HEADER = (
    "// Doors the Blender add-on made in %s: each door point's program (mod.json's \"programs\")\n"
    "// goes to its map. Edit the programs as you like; the add-on only appends to this file.\n"
    "#pragma target map %s\n"
    "\n"
    "#include <event.h>\n"
)
DOOR_PROGRAM = (
    "\n"
    "// The door %s: once it has opened and the screen faded, on to %s.\n"
    "NEW_PROGRAM(%s);\n"
    "PROGRAM(%s, %s);\n"
    "\n"
    "void %s(void)\n"
    "{\n"
    "    jump_to_map(-1, \"%s\", %s);\n"
    "    WAIT();\n"
    "}\n"
)


def add_door_program(mod_dir, map_name, point, target, program, arrive):
    """Appends the door's program to the mod's patch of the map's script (doors_MAP.c), made
    when missing; returns its path."""
    path = os.path.join(mod_dir, "doors_%s.c" % map_name)
    function = program.lower()
    text = ""
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
    if "NEW_PROGRAM(%s);" % program in text:
        return path
    if not text:
        text = DOORS_HEADER % (map_name, map_name)
    text += DOOR_PROGRAM % (point, target, program, program, function, function, target, arrive)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    return path


# the target menu's items, kept for Blender to read after the callback returns
_target_items = []


def target_maps(self, context):
    items = [(entry.code, entry.name, "") for entry in context.window_manager.dc2tk_maps.maps]
    mod_dir = prefs.absolute(prefs.get(context).mod_dir)
    if mod_dir:
        from . import testing

        for name in sorted(testing.new_maps(mod_dir)):
            items.insert(0, (name, "%s  (this mod's new map)" % name, ""))
    _target_items[:] = items or [("", "(read the map list first)", "")]
    return _target_items


class DC2TK_OT_add_door_to_map(bpy.types.Operator):
    """Add a door at the 3D cursor that takes the player to another map: the door point, a
    program in the mod's patch of this map's script that goes there, and mod.json's line giving
    the door that program"""

    bl_idname = "dc2tk.add_door_to_map"
    bl_label = "Door to Another Map"
    bl_options = {"REGISTER", "UNDO"}
    bl_property = "target"

    target: EnumProperty(name="To", items=target_maps)
    point: StringProperty(name="Door", description="The door point's name; empty for door_MAP")
    arrive: StringProperty(
        name="Arrive at",
        description="The program of the target map that runs on arrival: map_start, or a program of the mod",
        default="map_start",
    )

    @classmethod
    def poll(cls, context):
        return c2data.current_map_root(context) is not None

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self, width=360)

    def execute(self, context):
        root = c2data.current_map_root(context)
        name = c2data.map_name(root)
        target = import_map.map_code(self.target)
        if not target:
            self.report({"ERROR"}, "Choose the map the door goes to")
            return {"CANCELLED"}
        mod_dir = mod_dir_of(self, context)
        if mod_dir is None:
            return {"CANCELLED"}
        point = self.point or "door_%s" % target
        result = bpy.ops.dc2tk.add_point(preset="DOOR", name=point, in_placement=False)
        if "FINISHED" not in result:
            return {"CANCELLED"}
        added = context.active_object
        point = c2data.exact_name(added, "point")
        c2data.set_data_field(added, schema.data_field("event", "target"), target[:schema.TARGET_LIMIT])
        program = constant("ENTER_%s" % point)
        patch = add_door_program(mod_dir, name, point, target, program, self.arrive or "map_start")
        export_mod.update_mod_json(mod_dir, name)
        export_mod.set_programs(mod_dir, name, {point: program})
        self.report({"INFO"}, "Door %s to %s: program %s in %s" % (point, target, program, os.path.basename(patch)))
        return {"FINISHED"}


# --- an event point's program ---------------------------------------------------------------


class DC2TK_OT_point_program(bpy.types.Operator):
    """Give the active event point a program: one of the map script's, by number, or one of the
    mod's by name (written to mod.json's "programs", which build numbers)"""

    bl_idname = "dc2tk.point_program"
    bl_label = "Point Program"
    bl_options = {"REGISTER", "UNDO", "INTERNAL"}

    number: bpy.props.IntProperty(default=-1)
    name: StringProperty()

    def execute(self, context):
        obj = context.active_object
        if obj is None or c2data.lookup(obj, ("kind",)) != "event":
            return {"CANCELLED"}
        root = c2data.map_root(obj)
        point = c2data.exact_name(obj, "point")
        field = schema.data_field("event", "program")
        mod_dir = prefs.absolute(prefs.get(context).mod_dir).rstrip("\\/")
        if self.name:
            if not mod_dir:
                self.report({"ERROR"}, "Choose the mod folder first: a program by name is given in its mod.json")
                return {"CANCELLED"}
            export_mod.update_mod_json(mod_dir, c2data.map_name(root))
            export_mod.set_programs(mod_dir, c2data.map_name(root), {point: self.name})
            c2data.set_data_field(obj, field, 0)
            self.report({"INFO"}, "%s runs %s (numbered by build)" % (point, self.name))
            return {"FINISHED"}
        c2data.set_data_field(obj, field, max(0, self.number))
        if mod_dir and os.path.isfile(os.path.join(mod_dir, "mod.json")):
            if point in export_mod.programs_of(mod_dir, c2data.map_name(root)):
                export_mod.set_programs(mod_dir, c2data.map_name(root), {point: None})
        return {"FINISHED"}


class DC2TK_OT_name_program(bpy.types.Operator):
    """Give the active event point a program of the mod by name (a NEW_PROGRAM of its patches),
    written to mod.json's "programs" for build to number"""

    bl_idname = "dc2tk.name_program"
    bl_label = "Program by Name"
    bl_options = {"REGISTER", "UNDO"}

    name: StringProperty(name="Program", description="A NEW_PROGRAM name of the mod, or one the headers name")

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        if not self.name.strip():
            return {"CANCELLED"}
        return bpy.ops.dc2tk.point_program(name=self.name.strip())


class DC2TK_MT_programs(bpy.types.Menu):
    bl_idname = "DC2TK_MT_programs"
    bl_label = "Program"

    def draw(self, context):
        layout = self.layout
        report = scripts.info(c2data.current_map_root(context)) or {}
        programs = report.get("programs") or []
        if not programs:
            layout.label(text="Read Map Scripts for the map's programs", icon="INFO")
        for entry in programs:
            names = ", ".join(entry.get("names") or [])
            layout.operator(DC2TK_OT_point_program.bl_idname, text="%d  %s" % (entry["number"], names)).number = entry["number"]
        free = report.get("free_programs") or []
        if free:
            layout.separator()
            layout.label(text="Free for a mod: %s" % ", ".join(str(n) for n in free[:5]))
        layout.separator()
        layout.operator(DC2TK_OT_name_program.bl_idname, text="By name in mod.json...", icon="TEXT")
        layout.operator(DC2TK_OT_point_program.bl_idname, text="None (0)").number = 0


# --- a map of the mod's own ------------------------------------------------------------------

NEW_SCRIPT = (
    "// The event script of %s, a new map of the mod made from %s by the Blender add-on's New\n"
    "// Map. It is a whole map script: its PROGRAM table, MAIN and functions\n"
    "// (docs/modding/maps.md, \"A new map's own script\").\n"
    "\n"
    "#include <event.h>\n"
    "\n"
    "PROGRAM(map_start, arrive);\n"
    "MAIN(do_nothing);\n"
    "\n"
    "void do_nothing(void)\n"
    "{\n"
    "}\n"
    "\n"
    "// Every way in runs this unless it names another program: the player stands at the map's start.\n"
    "void arrive(void)\n"
    "{\n"
    "    init_event();\n"
    "    place_chara(CHARA_PLAYER, %s, %s, %s, %s);\n"
    "    reset_camera(CAMERA_RESET_BEHIND, 0.0, 10.0);\n"
    "    fade_in(30);\n"
    "}\n"
)


def rename_map(context, root, name):
    """Renames the map in the scene: its root, collision collections and world."""
    old = c2data.map_name(root)
    from . import import_map as imported, look

    collections = imported.collision_collections(context, root, create=False)
    c2data.assign(root, ("name",), name)
    root.name = schema.PREFIX["map"] + name
    for kind, label in (("collision", "collision"), ("camera", "camera collision")):
        found = collections.get(kind)
        if found is not None:
            found.name = "%s %s" % (name, label)
    world = bpy.data.worlds.get("c2 %s" % old)
    if world is not None:
        world.name = "c2 %s" % name
    for obj in [obj for obj in bpy.data.objects if obj.name.startswith(look.SUN_PREFIX % old)]:
        obj.name = obj.name.replace(look.SUN_PREFIX % old, look.SUN_PREFIX % name, 1)


def empty_map(context, root):
    """Removes every placement and the map's own points, leaving the settings, light sets and
    cameras: a map to build from scratch."""
    gone = []
    for placement in c2data.placements(root):
        gone.extend(c2data.descendants(placement))
    group = c2data.group_node(root, "points")
    if group is not None:
        for child in list(group.children):
            gone.extend(c2data.descendants(child))
    bpy.data.batch_remove(list(set(gone)))


def ground(context, root, size):
    """A square ground of the given size at the 3D cursor, made a model with floor collision."""
    import bmesh

    mesh = bpy.data.meshes.new("ground")
    shape = bmesh.new()
    try:
        bmesh.ops.create_grid(shape, x_segments=8, y_segments=8, size=size / 2.0)
        shape.to_mesh(mesh)
    finally:
        shape.free()
    obj = bpy.data.objects.new("ground", mesh)
    context.collection.objects.link(obj)
    obj.location = context.scene.cursor.location
    for other in context.selected_objects:
        other.select_set(False)
    obj.select_set(True)
    context.view_layer.objects.active = obj
    bpy.ops.dc2tk.new_model(name="%s_ground" % c2data.map_name(root), collision=True, camera=False)


class DC2TK_OT_new_map(bpy.types.Operator):
    """Make the map in the scene a new map of the mod (a copy of this one, under its own name):
    its line in mod.json, a script of its own that stands the player at the 3D cursor, and
    optionally an empty start with a ground to build on"""

    bl_idname = "dc2tk.new_map"
    bl_label = "New Map from This One"
    bl_options = {"REGISTER", "UNDO"}

    name: StringProperty(name="Name", description="Lower-case letters, digits and _, from a letter, 12 at most")
    title: StringProperty(name="Title", description="The map's title in the game's map table (printable ASCII)")
    script: BoolProperty(name="Script of its own", default=True)
    start_at: BoolProperty(name="New Game starts there", description="The mod's dev start_at (--dev builds only)", default=True)
    empty: BoolProperty(
        name="Start empty",
        description="Remove every placement and point (the light sets, fog and cameras stay) and lay a ground with collision",
        default=False,
    )
    ground_size: bpy.props.FloatProperty(name="Ground size", default=2000.0, min=100.0)

    @classmethod
    def poll(cls, context):
        return c2data.current_map_root(context) is not None

    def invoke(self, context, event):
        root = c2data.current_map_root(context)
        if not self.name:
            self.name = (c2data.map_name(root) + "x")[:12]
        return context.window_manager.invoke_props_dialog(self, width=360)

    def problem(self, context, like):
        if not schema.NEW_MAP_RE.match(self.name):
            return "A new map's name is lower-case letters, digits and _, from a letter, 12 at most"
        known = {entry.code for entry in context.window_manager.dc2tk_maps.maps}
        if self.name in known or self.name == like:
            return "The disc has a map %s" % self.name
        if any(ord(c) < 32 or ord(c) > 126 or c == '"' for c in self.title):
            return "The title is printable ASCII without quotes"
        return None

    def execute(self, context):
        root = c2data.current_map_root(context)
        like = c2data.map_name(root)
        problem = self.problem(context, like)
        if problem:
            self.report({"ERROR"}, problem)
            return {"CANCELLED"}
        mod_dir = mod_dir_of(self, context)
        if mod_dir is None:
            return {"CANCELLED"}
        from . import testing

        if like in testing.new_maps(mod_dir):
            self.report({"ERROR"}, "%s is already a new map of the mod; make new maps from the disc's" % like)
            return {"CANCELLED"}
        if self.empty:
            empty_map(context, root)
        rename_map(context, root, self.name)
        if self.empty:
            ground(context, root, self.ground_size)
            for other in context.selected_objects:
                other.select_set(False)
            context.view_layer.objects.active = root
            bpy.ops.dc2tk.add_point(preset="SPAWN", name="start", in_placement=False)
        path, data = export_mod.read_mod_json(mod_dir)
        added = data.get("add") if isinstance(data.get("add"), dict) else OrderedDict()
        # compact: what is deleted from a new map is left out, no script numbering its slots
        line = OrderedDict([("file", export_mod.mod_json_value(self.name)), ("like", like), ("compact", True)])
        if self.title:
            line["title"] = self.title
        if self.script:
            script = "%s.c" % self.name
            line["script"] = script
            world = context.scene.cursor.matrix.copy()
            x, y, z = (tools.number(v) for v in tools.game_position(world))
            yaw = tools.number(tools.game_rotation(world)[1])
            script_path = os.path.join(mod_dir, script)
            if not os.path.exists(script_path):
                with open(script_path, "w", encoding="utf-8", newline="\n") as handle:
                    handle.write(NEW_SCRIPT % (self.name, like, x, y, z, yaw))
        added["map:%s" % self.name] = line
        data["add"] = added
        if self.start_at:
            dev = data.get("dev") if isinstance(data.get("dev"), dict) else OrderedDict()
            dev["start_at"] = self.name
            data["dev"] = dev
        export_mod.write_mod_json(path, data)
        try:
            export_mod.export_map(context, c2data.current_map_root(context) or root, mod_dir)
        except (OSError, ValueError, RuntimeError) as error:
            self.report({"WARNING"}, "Made the map's line, but the export failed: %s" % error)
        self.report({"INFO"}, "%s is a new map of the mod, like %s" % (self.name, like))
        return {"FINISHED"}


classes = (
    DC2TK_OT_add_door_to_map,
    DC2TK_OT_point_program,
    DC2TK_OT_name_program,
    DC2TK_MT_programs,
    DC2TK_OT_new_map,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
