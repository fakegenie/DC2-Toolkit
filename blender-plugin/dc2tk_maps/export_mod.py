import ast
import json
import os
from collections import OrderedDict

import bpy

from . import c2data, import_map, look, prefs, runner, schema


def export_kwargs(path):
    wanted = {
        "filepath": path,
        "export_format": "GLTF_SEPARATE",
        "export_yup": True,
        "export_extras": True,
        "use_selection": True,
        "use_visible": False,
        "use_renderable": False,
        "use_active_collection": False,
        "export_apply": False,
        "export_lights": True,
        "export_cameras": False,
        "export_animations": False,
        "export_skins": False,
        "export_morph": False,
        "export_texture_dir": "textures",
        "export_image_format": "AUTO",
        "export_materials": "EXPORT",
        "export_vertex_color": "ACTIVE",
        "export_all_vertex_colors": True,
        "export_active_vertex_color_when_no_material": True,
        "export_colors": True,
        "export_normals": True,
        "export_texcoords": True,
        "export_hierarchy_flatten_objs": False,
        "export_hierarchy_full_collections": False,
        "export_import_convert_lighting_mode": "RAW",
        "export_unused_images": False,
        "check_existing": False,
        "will_save_settings": False,
    }
    return import_map.operator_kwargs(bpy.ops.export_scene.gltf, wanted)


def parent_layer(layer_root, wanted):
    stack = [layer_root]
    while stack:
        layer = stack.pop()
        for child in layer.children:
            if child == wanted:
                return layer if layer != layer_root else None
            stack.append(child)
    return None


def export_state(context):
    active = context.view_layer.objects.active
    return {
        "layers": [],
        "objects": [],
        "selected": [obj.name for obj in context.selected_objects],
        "active": active.name if active else None,
        "materials": [],
    }


def select_export_objects(context, objects, state):
    layer_root = context.view_layer.layer_collection
    seen = set()
    for obj in objects:
        for collection in obj.users_collection:
            layer = import_map.find_layer_collection(layer_root, collection)
            while layer is not None and layer != layer_root and layer.name not in seen:
                seen.add(layer.name)
                state["layers"].append((layer, layer.exclude, layer.hide_viewport))
                layer = parent_layer(layer_root, layer)
    for layer, _, _ in state["layers"]:
        if layer.exclude:
            layer.exclude = False
        layer.hide_viewport = False
    for obj in objects:
        hidden = obj.hide_get() if obj.name in context.view_layer.objects else False
        state["objects"].append((obj.name, hidden, obj.hide_viewport, obj.hide_select))
        obj.hide_viewport = False
        obj.hide_select = False
        if obj.name in context.view_layer.objects:
            obj.hide_set(False)
    for obj in context.selected_objects:
        obj.select_set(False)
    for obj in objects:
        if obj.name in context.view_layer.objects:
            obj.select_set(True)


def restore_export_state(context, state):
    look.restore_materials(state["materials"])
    for obj in context.selected_objects:
        obj.select_set(False)
    for name, hidden, hide_viewport, hide_select in state["objects"]:
        obj = bpy.data.objects.get(name)
        if obj is None:
            continue
        obj.hide_viewport = hide_viewport
        obj.hide_select = hide_select
        if obj.name in context.view_layer.objects:
            obj.hide_set(hidden)
    for name in state["selected"]:
        obj = bpy.data.objects.get(name)
        if obj is not None and obj.name in context.view_layer.objects and obj.visible_get():
            obj.select_set(True)
    if state["active"] and state["active"] in context.view_layer.objects:
        context.view_layer.objects.active = context.view_layer.objects[state["active"]]
    for layer, exclude, hide_viewport in reversed(state["layers"]):
        layer.hide_viewport = hide_viewport
        if exclude:
            layer.exclude = True


def image_picture(gltf, material):
    texture = material.get("pbrMetallicRoughness", {}).get("baseColorTexture")
    if not isinstance(texture, dict):
        return None
    try:
        image = gltf["images"][gltf["textures"][texture["index"]]["source"]]
    except (KeyError, IndexError, TypeError):
        return None
    name = image.get("name") or os.path.splitext(os.path.basename(image.get("uri", "")))[0]
    return schema.strip_suffix(name) or None


def fix_gltf(path):
    with open(path, "r", encoding="utf-8") as handle:
        gltf = json.load(handle, object_pairs_hook=OrderedDict)
    changed = 0
    for node in gltf.get("nodes", []):
        extras = node.get("extras")
        if not isinstance(extras, dict) or c2data.KEY not in extras:
            continue
        value = extras[c2data.KEY]
        if isinstance(value, str):
            try:
                parsed = ast.literal_eval(value)
            except (ValueError, SyntaxError):
                parsed = None
            if isinstance(parsed, dict):
                extras[c2data.KEY] = parsed
        name = node.get("name")
        if name and schema.strip_suffix(name) != name:
            node["name"] = schema.strip_suffix(name)
            changed += 1
    for scene in gltf.get("scenes", []):
        for index in scene.get("nodes", []):
            name = gltf["nodes"][index].get("name", "")
            if name.startswith(schema.PREFIX["map"]):
                scene["name"] = schema.game_name(name, "map")
    for material in gltf.get("materials", []):
        name = material.get("name", "")
        if schema.parse_col(name) is not None and schema.strip_suffix(name) != name:
            material["name"] = schema.strip_suffix(name)
            changed += 1

        picture = image_picture(gltf, material)
        extras = material.get("extras")
        if picture and isinstance(extras, dict) and extras.get("texture") not in (None, picture):
            extras["texture"] = picture
            changed += 1
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(gltf, handle, ensure_ascii=False, indent=1)
        handle.write("\n")
    return changed


def mod_json_key(name):
    return "map:%s" % name


def mod_json_value(name):
    return "maps/%s/%s.gltf" % (name, name)


def adds_file(data, value):
    added = data.get("add")
    if not isinstance(added, dict):
        return None
    for key, entry in added.items():
        if isinstance(entry, dict) and str(entry.get("file", "")).replace("\\", "/") == value:
            return key
    return None


def default_id(folder):
    name = ""
    for c in folder.lower():
        if c.isascii() and c.isalnum():
            name += c
        elif name and not name.endswith("-"):
            name += "-"
    return "local." + (name.rstrip("-") or "mod")


def read_mod_json(mod_dir):
    path = os.path.join(mod_dir, "mod.json")
    data = OrderedDict()
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as handle:
            text = handle.read()
        if text.strip():
            data = json.loads(text, object_pairs_hook=OrderedDict)
        if not isinstance(data, dict):
            raise ValueError("%s is not a JSON object" % path)
    if not data:
        folder = os.path.basename(os.path.normpath(mod_dir))
        data = OrderedDict([("format", 2), ("id", default_id(folder)), ("version", "0.1.0")])
    return path, data


def write_mod_json(path, data):
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(data, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def map_line(data, name):
    """The mod's line for map name: (the section, its key, its value), from "add" when a line
    there names the map's scene, else "replace"; the value None when there is none yet."""
    key = adds_file(data, mod_json_value(name))
    if key is not None:
        return "add", key, data["add"][key]
    replace = data.get("replace")
    if isinstance(replace, dict) and mod_json_key(name) in replace:
        return "replace", mod_json_key(name), replace[mod_json_key(name)]
    return "replace", mod_json_key(name), None


def update_mod_json(mod_dir, name):
    path, data = read_mod_json(mod_dir)
    if adds_file(data, mod_json_value(name)) is not None:
        return path
    replace = data.get("replace")
    if replace is None:
        replace = OrderedDict()
        data["replace"] = replace
    if not isinstance(replace, dict):
        raise ValueError("\"replace\" in %s is not an object" % path)
    line = replace.get(mod_json_key(name))
    if isinstance(line, dict):
        # a line with more than the scene ("programs"): its scene is the export, the rest kept
        line["file"] = mod_json_value(name)
    else:
        replace[mod_json_key(name)] = mod_json_value(name)
    write_mod_json(path, data)
    return path


def set_programs(mod_dir, name, programs):
    """Gives event points of map name programs by name or number, in the map's line's
    "programs" (made an object when it is only the scene's path)."""
    path, data = read_mod_json(mod_dir)
    section, key, line = map_line(data, name)
    if not isinstance(data.get(section), dict):
        data[section] = OrderedDict()
    if not isinstance(line, dict):
        line = OrderedDict([("file", line if isinstance(line, str) else mod_json_value(name))])
    given = line.get("programs")
    if not isinstance(given, dict):
        given = OrderedDict()
    for point, program in programs.items():
        if program in (None, ""):
            given.pop(point, None)
        else:
            given[point] = program
    if given:
        line["programs"] = given
    else:
        line.pop("programs", None)
    data[section][key] = line
    write_mod_json(path, data)
    return path


def programs_of(mod_dir, name):
    """The programs the mod's line for map name gives its event points."""
    try:
        _path, data = read_mod_json(mod_dir)
    except (OSError, ValueError):
        return {}
    _section, _key, line = map_line(data, name)
    given = line.get("programs") if isinstance(line, dict) else None
    return dict(given) if isinstance(given, dict) else {}


def export_map(context, root, mod_dir):
    from . import animate

    # what the animations moved goes back first: a played frame is never written
    animate.stop()
    name = c2data.map_name(root)
    folder = os.path.join(mod_dir, "maps", name)
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, name + ".gltf")
    if context.mode != "OBJECT" and context.active_object is not None:
        bpy.ops.object.mode_set(mode="OBJECT")
    # what the scene only shows (the sky) stays out of the mod
    objects = c2data.map_objects(root)
    state = export_state(context)
    try:
        select_export_objects(context, objects, state)
        look.export_materials(state["materials"])
        result = bpy.ops.export_scene.gltf(**export_kwargs(path))
    finally:
        restore_export_state(context, state)
    if "FINISHED" not in result:
        raise RuntimeError("Blender's glTF exporter failed")
    fixed = fix_gltf(path)
    mod_json = update_mod_json(mod_dir, name)
    return path, mod_json, fixed


class DC2TK_OT_export_mod(bpy.types.Operator):
    """Export the map (glTF Separate, +Y up, custom properties) into the mod as
    maps/NAME/NAME.gltf and add its line to the mod's mod.json"""

    bl_idname = "dc2tk.export_mod"
    bl_label = "Export Map to Mod"
    bl_options = {"REGISTER"}

    mod_dir: bpy.props.StringProperty(
        name="Mod folder", description="Empty for the add-on preferences' mod folder", subtype="DIR_PATH"
    )
    validate: bpy.props.BoolProperty(name="Validate first", default=True)

    @classmethod
    def poll(cls, context):
        return c2data.current_map_root(context) is not None

    def execute(self, context):
        from . import validate

        root = c2data.current_map_root(context)
        settings = prefs.get(context)
        mod_dir = prefs.absolute(self.mod_dir or settings.mod_dir)
        if not mod_dir:
            self.report({"ERROR"}, "Choose the mod folder (mods/NAME)")
            return {"CANCELLED"}
        if self.validate:
            validate.results[:] = validate.check(root)
            errors = [r for r in validate.results if r[0] == validate.ERROR]
            if errors:
                for level, name, message in errors[:10]:
                    self.report({"ERROR"}, "%s: %s" % (name, message))
                self.report({"ERROR"}, "%d validation errors: not exported" % len(errors))
                return {"CANCELLED"}

        for level, name, message in validate.left_out(root)[:10]:
            self.report({"WARNING"}, "%s: %s" % (name, message))
        try:
            path, mod_json, fixed = export_map(context, root, mod_dir)
        except (OSError, ValueError, RuntimeError) as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}
        self.report({"INFO"}, "Wrote %s and %s" % (path, mod_json))
        return {"FINISHED"}


def is_disc(path):
    """Whether dc2tk build can start from path: a disc image, or a folder dc2tk extract wrote
    (it holds manifest.json)."""
    return os.path.isfile(path) or os.path.isfile(os.path.join(path, "manifest.json"))


class DC2TK_OT_build(bpy.types.Operator):
    """Run dc2tk build with the mod: a new disc image (output shown in the panel and the text
    "dc2tk output")"""

    bl_idname = "dc2tk.build"
    bl_label = "Build ISO with Mod"
    bl_options = {"REGISTER"}

    def execute(self, context):
        return runner.execute(self, context)

    def modal(self, context, event):
        return runner.modal(self, context, event)

    def invoke(self, context, event):
        return runner.invoke(self, context)

    def command(self, context):
        settings = prefs.get(context)
        dc2tk = prefs.dc2tk_command(settings)
        if dc2tk is None:
            self.report({"ERROR"}, "dc2tk not found: set the repository (or dc2tk itself) in the add-on's preferences")
            return None
        mod_dir = prefs.absolute(settings.mod_dir).rstrip("\\/")
        source = prefs.source_dir(settings)
        iso = prefs.absolute(settings.iso_path)
        output = prefs.absolute(settings.output_iso)
        for label, value, check in (
            ("mod folder", mod_dir, os.path.isdir),
            ("source tree", source, os.path.isdir),
            ("retail disc (an image, or an extracted disc folder)", iso, is_disc),
        ):
            if not value or not check(value):
                self.report({"ERROR"}, "Set the %s (%s)" % (label, value or "empty"))
                return None
        if not output:
            self.report({"ERROR"}, "Set the output disc image")
            return None
        command = dc2tk + [
            "build",
            source,
            iso,
            "-o",
            output,
            "--mods",
            os.path.dirname(mod_dir),
            "--mod",
            os.path.basename(mod_dir),
        ]
        return ("dc2tk build", command, prefs.working_dir(settings))

    def finish(self, context, job):
        if job["returncode"] == 0:
            self.report({"INFO"}, "dc2tk build finished: %s" % prefs.absolute(prefs.get(context).output_iso))
            return {"FINISHED"}
        self.report({"ERROR"}, "dc2tk build failed (exit %s); see the text \"%s\"" % (job["returncode"], runner.TEXT_NAME))
        return {"CANCELLED"}


class DC2TK_OT_show_output(bpy.types.Operator):
    """Show dc2tk's whole output in a Text Editor"""

    bl_idname = "dc2tk.show_output"
    bl_label = "Show dc2tk Output"

    name: bpy.props.StringProperty(default=runner.TEXT_NAME)

    def execute(self, context):
        text = bpy.data.texts.get(self.name)
        if text is None:
            return {"CANCELLED"}
        for window in context.window_manager.windows:
            for area in window.screen.areas:
                if area.type == "TEXT_EDITOR":
                    area.spaces.active.text = text
                    return {"FINISHED"}
        bpy.ops.wm.window_new()
        area = context.window_manager.windows[-1].screen.areas[0]
        area.type = "TEXT_EDITOR"
        area.spaces.active.text = text
        return {"FINISHED"}


classes = (DC2TK_OT_export_mod, DC2TK_OT_build, DC2TK_OT_show_output)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
