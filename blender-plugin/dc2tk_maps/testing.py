"""The loop from an edit to the game: what the map's scripts know of it (Read Map Scripts), what a
build would change (Preview Changes, dc2tk build --dry-run), and a test image that starts a new
game on the map with the player at the 3D cursor (Test Here, dc2tk build --start-at)."""

import json
import os

import bpy
from bpy.props import EnumProperty

from . import budgets, c2data, export_mod, import_map, prefs, runner, schema, scripts, tools

TEST_MOD = "test-here"
TEST_MOD_ID = "local.test-here"
TEST_PROGRAM = "TEST_HERE"
PREVIEW_TEXT = budgets.PREVIEW_TEXT

# what the last preview said, for the Check panel: (kind, text)
preview = []


# --- the map's scripts ----------------------------------------------------------------------


def disc_dir(settings):
    """The disc dc2tk reads names and maps from: the retail disc set for building, else the
    repository's extracted disc."""
    if settings.iso_path:
        return prefs.absolute(settings.iso_path)
    root = prefs.repo(settings)
    return os.path.join(root, "extracted", "disc") if root else ""


def disc_names(path):
    """The placements (name and slot), pieces and points of a map as the disc has it, from its
    export: {"placements": {name: slot}, "pieces": {(placement, piece)}, "points": {(placement or
    "", point)}}."""
    gltf = import_map.read_gltf(path)
    nodes = gltf.get("nodes", [])
    found = {"placements": {}, "pieces": set(), "points": set()}

    def c2(node):
        value = (node.get("extras") or {}).get("c2")
        if isinstance(value, str):
            try:
                value = json.loads(value)
            except ValueError:
                value = None
        return value if isinstance(value, dict) else {}

    def walk(index, placement):
        node = nodes[index]
        data = c2(node)
        role = data.get("role")
        name = data.get("name") or schema.game_name(node.get("name", ""), role)
        if role == "placement":
            found["placements"][name] = data.get("slot")
            placement = name
        elif role == "piece":
            found["pieces"].add((placement, name))
        elif role == "point":
            found["points"].add((placement or "", name))
        for child in node.get("children") or []:
            walk(child, placement)

    scene = gltf.get("scenes", [{}])[gltf.get("scene", 0)] if gltf.get("scenes") else {}
    for index in scene.get("nodes", []):
        walk(index, None)
    return found


def on_map(use, names):
    """Whether a script's name is of this map as the disc has it (scripts shared by several maps
    name the others' parts too)."""
    what, part, name = use.get("what"), use.get("part"), use.get("name")
    slots = {slot: place for place, slot in names["placements"].items()}
    if isinstance(part, int):
        part = slots.get(part)
        if part is None:
            return False
    if what == "placement":
        return part in names["placements"]
    if what == "piece":
        return any(piece == name and (part is None or place == part) for place, piece in names["pieces"])
    if what == "point":
        return any(point == name and (part is None or place == part) for place, point in names["points"])
    return False


def read_report(context, name):
    """`dc2tk map SOURCE NAME --json`, or the reason it could not be read."""
    settings = prefs.get(context)
    dc2tk = prefs.dc2tk_command(settings)
    if dc2tk is None:
        return None, "dc2tk not found: set the repository (or dc2tk itself) in the add-on's preferences"
    command = dc2tk + ["map", prefs.source_dir(settings), name, "--json"]
    disc = disc_dir(settings)
    if disc:
        command += ["--disc", disc]
    code, output = runner.run_quick(command, prefs.working_dir(settings))
    if code != 0:
        last = (output or "").strip().splitlines()[-1:] or ["no output"]
        return None, "dc2tk map failed (exit %s): %s" % (code, last[0])
    start, end = output.find("{"), output.rfind("}")
    try:
        return json.loads(output[start:end + 1]), None
    except ValueError as error:
        return None, "dc2tk map wrote no JSON: %s" % error


class DC2TK_OT_read_scripts(bpy.types.Operator):
    """Read what the map's scripts know of it (dc2tk map --json): their programs, for doors and
    events; where they stand the player; and the placements, pieces and points they name, which
    must keep their names"""

    bl_idname = "dc2tk.read_scripts"
    bl_label = "Read Map Scripts"
    bl_options = {"REGISTER"}

    @classmethod
    def poll(cls, context):
        return c2data.current_map_root(context) is not None

    def execute(self, context):
        root = c2data.current_map_root(context)
        name = c2data.map_name(root)
        report, problem = read_report(context, name)
        if report is None:
            self.report({"ERROR"}, problem)
            return {"CANCELLED"}
        if not report.get("found"):
            report = {"map": name, "found": False, "programs": [], "free_programs": [], "player_positions": [], "names": []}
        export = import_map.find_export(os.path.join(prefs.maps_dir(prefs.get(context)), name), name)
        if export is not None:
            names = disc_names(export)
            report["names"] = [use for use in report.get("names") or [] if on_map(use, names)]
        scripts.store(name, report)
        from . import overlay

        overlay.invalidate()
        self.report(
            {"INFO"},
            "%s: %d programs, %d player spots, %d names its scripts use"
            % (name, len(report.get("programs") or []), len(report.get("player_positions") or []), len(report.get("names") or [])),
        )
        return {"FINISHED"}


# --- what a build would do -------------------------------------------------------------------


def build_inputs(operator, context):
    """The mod, source tree and retail disc a build needs, or None after reporting what is
    missing."""
    settings = prefs.get(context)
    dc2tk = prefs.dc2tk_command(settings)
    if dc2tk is None:
        operator.report({"ERROR"}, "dc2tk not found: set the repository (or dc2tk itself) in the add-on's preferences")
        return None
    mod_dir = prefs.absolute(settings.mod_dir).rstrip("\\/")
    source = prefs.source_dir(settings)
    disc = prefs.absolute(settings.iso_path) or disc_dir(settings)
    for label, value, check in (
        ("mod folder", mod_dir, os.path.isdir),
        ("source tree", source, os.path.isdir),
        ("retail disc (an image, or an extracted disc folder)", disc, export_mod.is_disc),
    ):
        if not value or not check(value):
            operator.report({"ERROR"}, "Set the %s (%s)" % (label, value or "empty"))
            return None
    return dc2tk, mod_dir, source, disc


def export_checked(operator, context, mod_dir):
    """Validates and exports the map into the mod; False after reporting why not."""
    from . import animate, validate

    animate.stop()
    root = c2data.current_map_root(context)
    validate.results[:] = validate.check(root)
    errors = [r for r in validate.results if r[0] == validate.ERROR]
    if errors:
        for _level, name, message in errors[:10]:
            operator.report({"ERROR"}, "%s: %s" % (name, message))
        operator.report({"ERROR"}, "%d validation errors: not exported" % len(errors))
        return False
    try:
        export_mod.export_map(context, root, mod_dir)
    except (OSError, ValueError, RuntimeError) as error:
        operator.report({"ERROR"}, str(error))
        return False
    return True


def summarise(lines):
    """What a build said that a map editor wants to see: its notes, what it made of the maps,
    the files it replaced or added, and the memory it checked."""
    found = []
    for line in lines:
        text = line.strip()
        if not text:
            continue
        if text.startswith("error"):
            found.append(("ERROR", text))
        elif text.startswith("warning"):
            found.append(("WARNING", text))
        elif text.startswith("note: map:"):
            found.append(("NOTE", text.split(": ", 2)[-1]))
        elif text.startswith(("replaced:", "added:")):
            found.append(("FILE", text))
        elif text.startswith("memory: map "):
            found.append(("MEMORY", text[len("memory: "):]))
        elif line.startswith("  ") and "(map " in text:
            found.append(("MAP", text.split("): ", 1)[-1]))
        elif text.startswith(("dry run:", "start at", "the image was written")) or " written to " in text:
            found.append(("DONE", text))
    return found


class DC2TK_OT_preview_changes(bpy.types.Operator):
    """Export the map into the mod and run dc2tk build --dry-run: what the build would change
    (templates split, models rebuilt and added, pictures repainted and added) and the memory the
    map would use, without writing an image"""

    bl_idname = "dc2tk.preview_changes"
    bl_label = "Preview Changes"
    bl_options = {"REGISTER"}

    @classmethod
    def poll(cls, context):
        return c2data.current_map_root(context) is not None

    def execute(self, context):
        return runner.execute(self, context)

    def modal(self, context, event):
        return runner.modal(self, context, event)

    def invoke(self, context, event):
        return runner.invoke(self, context)

    def command(self, context):
        found = build_inputs(self, context)
        if found is None:
            return None
        dc2tk, mod_dir, source, disc = found
        if not export_checked(self, context, mod_dir):
            return None
        command = dc2tk + [
            "build", source, disc, "--dry-run", "--mods", os.path.dirname(mod_dir), "--mod", export_mod.mod_id(mod_dir),
        ]
        return ("dc2tk build --dry-run", command, prefs.working_dir(prefs.get(context)))

    def finish(self, context, job):
        lines = runner.output_lines(job)
        text = bpy.data.texts.get(PREVIEW_TEXT) or bpy.data.texts.new(PREVIEW_TEXT)
        text.clear()
        text.write("\n".join(lines) + "\n")
        preview[:] = summarise(lines)
        if job["returncode"] != 0:
            self.report({"ERROR"}, "The build would fail (exit %s); see the text \"%s\"" % (job["returncode"], PREVIEW_TEXT))
            return {"CANCELLED"}
        self.report({"INFO"}, "Preview: %d changes noted (text \"%s\")" % (len(preview), PREVIEW_TEXT))
        return {"FINISHED"}


# --- a test image ---------------------------------------------------------------------------


def test_iso(settings):
    output = prefs.absolute(settings.output_iso)
    if output:
        stem, _ = os.path.splitext(output)
        return stem + "-test.iso"
    root = prefs.repo(settings)
    return os.path.join(root, "test-here.iso") if root else ""


def test_script(map_name, world):
    x, y, z = (tools.number(v) for v in tools.game_position(world))
    yaw = tools.number(tools.game_rotation(world)[1])
    return (
        "// Made by the Blender add-on's Test Here, rewritten each time: a new game starts on %s\n"
        "// with the player at the 3D cursor. Not for playing: use it to try the map.\n"
        "#pragma target map %s\n"
        "\n"
        "#include <event.h>\n"
        "\n"
        "NEW_PROGRAM(%s);\n"
        "PROGRAM(%s, test_here);\n"
        "\n"
        "void test_here(void)\n"
        "{\n"
        "    init_event();\n"
        "    place_chara(CHARA_PLAYER, %s, %s, %s, %s);\n"
        "    reset_camera(CAMERA_RESET_BEHIND, 0.0, 10.0);\n"
        "    fade_in(30);\n"
        "}\n" % (map_name, map_name, TEST_PROGRAM, TEST_PROGRAM, x, y, z, yaw)
    )


def write_test_mod(mods_dir, map_name, world):
    """The test mod beside the user's: a patch of the map's script with a program that stands the
    player where wanted."""
    folder = os.path.join(mods_dir, TEST_MOD)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "mod.json"), "w", encoding="utf-8", newline="\n") as handle:
        body = {"format": 2, "id": TEST_MOD_ID, "version": "1.0.0", "name": "Test here (made by the Blender add-on)"}
        json.dump(body, handle, indent=2)
        handle.write("\n")
    for old in os.listdir(folder):
        if old.endswith(".c"):
            os.remove(os.path.join(folder, old))
    with open(os.path.join(folder, "test_here.c"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(test_script(map_name, world))
    return folder


def new_maps(mod_dir):
    """The maps the mod adds (its "add" map: lines), by name."""
    path = os.path.join(mod_dir, "mod.json")
    try:
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, ValueError):
        return set()
    added = data.get("add") if isinstance(data, dict) else None
    return {key[4:] for key in added if key.startswith("map:")} if isinstance(added, dict) else set()


class DC2TK_OT_test_here(bpy.types.Operator):
    """Export the map into the mod and build a test image whose new game starts on this map with
    the player at the 3D cursor (or the active object), facing its way"""

    bl_idname = "dc2tk.test_here"
    bl_label = "Test Here"
    bl_options = {"REGISTER"}

    source: EnumProperty(
        name="Stand at",
        items=(("CURSOR", "3D cursor", ""), ("ACTIVE", "Active object", "")),
        default="CURSOR",
    )

    @classmethod
    def poll(cls, context):
        return c2data.current_map_root(context) is not None

    def execute(self, context):
        return runner.execute(self, context)

    def modal(self, context, event):
        return runner.modal(self, context, event)

    def invoke(self, context, event):
        return runner.invoke(self, context)

    def command(self, context):
        found = build_inputs(self, context)
        if found is None:
            return None
        dc2tk, mod_dir, source, disc = found
        settings = prefs.get(context)
        output = test_iso(settings)
        if not output:
            self.report({"ERROR"}, "Set the output disc image (the test image goes beside it)")
            return None
        root = c2data.current_map_root(context)
        name = c2data.map_name(root)
        if not export_checked(self, context, mod_dir):
            return None
        obj = context.active_object
        world = obj.matrix_world.copy() if self.source == "ACTIVE" and obj is not None else context.scene.cursor.matrix.copy()
        mods_dir = os.path.dirname(mod_dir)
        command = dc2tk + ["build", source, disc, "-o", output, "--mods", mods_dir, "--mod", export_mod.mod_id(mod_dir)]
        report = scripts.info(root)
        has_script = bool(report.get("scripts")) if report else True
        if name in new_maps(mod_dir) or not has_script:
            # a new map's script is the mod's own, and a map without one has none to patch: the
            # game starts at its map_start
            command += ["--start-at", name]
            self.report({"WARNING"}, "%s starts where its map_start puts the player" % name)
        else:
            write_test_mod(mods_dir, name, world)
            command += ["--mod", TEST_MOD_ID, "--start-at", "%s:%s" % (name, TEST_PROGRAM)]
        if context.window_manager.dc2tk_maps.test_play:
            command.append("--play")
        self._output = output
        return ("dc2tk build (test here)", command, prefs.working_dir(settings))

    def finish(self, context, job):
        lines = runner.output_lines(job)
        preview[:] = summarise(lines)
        if job["returncode"] == 0:
            self.report({"INFO"}, "Test image: %s" % self._output)
            return {"FINISHED"}
        self.report({"ERROR"}, "dc2tk build failed (exit %s); see the text \"%s\"" % (job["returncode"], runner.TEXT_NAME))
        return {"CANCELLED"}


def draw_preview(layout):
    if not preview:
        return
    box = layout.box()
    box.label(text="Last build said", icon="TEXT")
    col = box.column(align=True)
    icons = {"ERROR": "ERROR", "WARNING": "ERROR", "NOTE": "INFO", "FILE": "FILE", "MEMORY": "MEMORY", "MAP": "WORLD", "DONE": "CHECKMARK"}
    for kind, text in preview[:16]:
        row = col.row()
        row.alert = kind == "ERROR"
        row.label(text=text, icon=icons.get(kind, "DOT"))
    if len(preview) > 16:
        col.label(text="... %d more (text \"%s\")" % (len(preview) - 16, PREVIEW_TEXT))


classes = (DC2TK_OT_read_scripts, DC2TK_OT_preview_changes, DC2TK_OT_test_here)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
