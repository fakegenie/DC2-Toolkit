import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

import bpy

from . import budgets, c2data, export_mod, prefs, runner, schema, tools, validate

START_SECONDS = 120.0
PRESS_SECONDS = 1.5
LOG = "blender-game.log"

status = {"text": ""}
WRITTEN = re.compile(r"(\d+) game files in .* \((\d+) written, (\d+) removed\)")


def build_summary(lines):
    """What the build left to say: (nothing reached the game, its notes and warnings)."""
    unchanged = any((found := WRITTEN.search(line)) and found.group(2) == "0" and found.group(3) == "0" for line in lines)
    said = [line for line in lines if line.startswith(("note:", "warning:"))]
    return unchanged, said


def port_binary(settings):
    if settings.port_binary:
        return prefs.absolute(settings.port_binary)
    root = prefs.repo(settings)
    name = os.path.join("windows", "dc2-debug.exe") if sys.platform.startswith("win") else "dc2-debug"
    return os.path.join(root, "build", "port", name) if root else ""


def overrides_dir(settings):
    if settings.overrides_dir:
        return prefs.absolute(settings.overrides_dir)
    binary = port_binary(settings)
    return os.path.join(os.path.dirname(binary), "overrides") if binary else ""


def ask(settings, request, timeout=3.0):
    url = "http://127.0.0.1:%d/%s" % (settings.debug_port, request)
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            return response.status, response.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode("utf-8", errors="replace")
    except (OSError, ValueError):
        return None, ""


def running(settings):
    return ask(settings, "status", timeout=0.5)[0] == 200


def launch(settings, disc):
    binary = port_binary(settings)
    folder = os.path.dirname(binary)
    env = dict(os.environ, DC2TK_OVERRIDES=overrides_dir(settings), DC2TK_DEBUG_PORT=str(settings.debug_port))
    log = open(os.path.join(folder, LOG), "w")
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if sys.platform.startswith("win") else 0
    subprocess.Popen(
        [binary, disc], cwd=prefs.working_dir(settings), env=env, stdout=log, stderr=log,
        stdin=subprocess.DEVNULL, creationflags=flags,
    )
    log.close()


def is_dungeon(root):
    return budgets.figures(root)["templates"]


def enter_request(context, root, start):
    name = c2data.map_name(root)
    if is_dungeon(root):
        return "reload"
    if start == "PLAYER":
        return "reload?map=%s" % urllib.parse.quote(name)
    world = context.scene.cursor.matrix
    x, y, z = tools.game_position(world)
    yaw = tools.game_rotation(world)[1]
    query = urllib.parse.urlencode({
        "map": name, "x": "%.2f" % (round(x, 2) + 0.0), "y": "%.2f" % (round(y, 2) + 0.0),
        "z": "%.2f" % (round(z, 2) + 0.0), "yaw": "%.4f" % (round(yaw, 4) + 0.0),
    })
    return "reload?" + query


def on_map(settings, name):
    code, body = ask(settings, "status", timeout=1.0)
    return code == 200 and ('"map_name": "%s"' % name) in body


def watch_command(settings, source, disc, mod_dir, request):
    command = prefs.dc2tk_command(settings) + [
        "mod", "watch", source, disc, export_mod.mod_id(mod_dir), "--mods", os.path.dirname(mod_dir),
        "--overrides", overrides_dir(settings), "--port", str(settings.debug_port), "--once",
    ]
    if request and "?" in request:
        command += ["--reload", request.partition("?")[2]]
    return command


def problems(settings, mod_dir):
    disc = prefs.absolute(settings.iso_path)
    if not mod_dir:
        return "Choose the mod folder (mods/NAME)"
    if not prefs.dc2tk_command(settings):
        return "dc2tk is not found: set the repository or the dc2tk executable in the preferences"
    if not os.path.isfile(disc):
        return "Set the retail disc to the game's ISO: the port plays an image, not an extracted folder"
    if not os.path.isfile(port_binary(settings)):
        return "The debug port is not built: python3 -I tools/port.py --movies --debug"
    return None


class DC2TK_OT_play_in_game(bpy.types.Operator):
    """Export the map into the mod, build it into the native port's override folder and show it in the
    running debug port (started when it is not running), with the player at the 3D cursor"""

    bl_idname = "dc2tk.play_in_game"
    bl_label = "Play in Game"
    bl_options = {"REGISTER"}

    @classmethod
    def poll(cls, context):
        return c2data.current_map_root(context) is not None

    def invoke(self, context, event):
        settings = prefs.get(context)
        root = c2data.current_map_root(context)
        mod_dir = prefs.absolute(settings.mod_dir)
        wrong = problems(settings, mod_dir)
        if wrong:
            self.report({"ERROR"}, wrong)
            return {"CANCELLED"}
        validate.results[:] = validate.check(root)
        errors = [result for result in validate.results if result[0] == validate.ERROR]
        if errors:
            for level, name, message in errors[:10]:
                self.report({"ERROR"}, "%s: %s" % (name, message))
            self.report({"ERROR"}, "%d validation errors: not exported" % len(errors))
            return {"CANCELLED"}
        try:
            export_mod.export_map(context, root, mod_dir)
        except (OSError, ValueError, RuntimeError) as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}
        disc = prefs.absolute(settings.iso_path)
        self._request = enter_request(context, root, context.window_manager.dc2tk_maps.play_start)
        self._name = c2data.map_name(root)
        self._started = not running(settings)
        if self._started:
            launch(settings, disc)
        elif self._request.startswith("reload?map=") and "&x=" not in self._request and on_map(settings, self._name):
            self._request = "reload"
        self._job = runner.new_job(
            "dc2tk mod watch (play in game)",
            watch_command(settings, prefs.source_dir(settings), disc, mod_dir, None if self._started else self._request),
            prefs.working_dir(settings),
        )
        runner.current = self._job
        try:
            runner.start_job(self._job)
        except OSError as error:
            self.report({"ERROR"}, "Could not run dc2tk: %s" % error)
            return {"CANCELLED"}
        status["text"] = "Building the map into the game..."
        self._entering = False
        self._since = time.monotonic()
        self._pressed = 0.0
        self._timer = context.window_manager.event_timer_add(0.25, window=context.window)
        context.window_manager.modal_handler_add(self)
        return {"RUNNING_MODAL"}

    def execute(self, context):
        return self.invoke(context, None)

    def modal(self, context, event):
        if event.type != "TIMER":
            return {"PASS_THROUGH"}
        for area in context.screen.areas if context.screen else []:
            if area.type == "VIEW_3D":
                area.tag_redraw()
        if not self._entering:
            if not runner.job_done(self._job):
                return {"PASS_THROUGH"}
            runner.write_text(self._job)
            if self._job["returncode"] != 0 or any(line.startswith("not rebuilt") for line in runner.output_lines(self._job)):
                return self.end(context, {"ERROR"}, "The build failed; see the text \"%s\"" % runner.TEXT_NAME)
            unchanged, said = build_summary(runner.output_lines(self._job))
            for line in said[:5]:
                self.report({"WARNING"}, line)
            if unchanged:
                self.report({"WARNING"}, "The game gets nothing new: the mod builds to the same files as before")
            if not self._started and "the game reloaded" in (runner.output_lines(self._job) or [""])[-1]:
                return self.end(context, {"INFO"}, "In the game on %s%s" % (self._name, " (nothing changed)" if unchanged else ""))
            self._entering = True
            self._since = time.monotonic()
            status["text"] = "Starting the game..."
        return self.enter(context)

    def enter(self, context):
        settings = prefs.get(context)
        if time.monotonic() - self._since > START_SECONDS:
            return self.end(context, {"ERROR"}, "The game did not get to %s; see build/port/%s" % (self._name, LOG))
        code, body = ask(settings, self._request, timeout=10.0)
        if code == 200:
            return self.end(context, {"INFO"}, "In the game on %s" % self._name)
        if "still starting" in body or code is None:
            if code is not None and time.monotonic() - self._pressed > PRESS_SECONDS:
                ask(settings, "press?buttons=cross&frames=4")
                self._pressed = time.monotonic()
            return {"PASS_THROUGH"}
        return self.end(context, {"ERROR"}, "The game refused: %s" % body.strip())

    def end(self, context, level, message):
        context.window_manager.event_timer_remove(self._timer)
        status["text"] = message
        self.report(level, message)
        return {"FINISHED"} if "ERROR" not in level else {"CANCELLED"}


class DC2TK_OT_cursor_to_player(bpy.types.Operator):
    """Put the 3D cursor where the player stands in the running debug port, and show it"""

    bl_idname = "dc2tk.cursor_to_player"
    bl_label = "Cursor to Player"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        settings = prefs.get(context)
        code, body = ask(settings, "overrides")
        try:
            player = json.loads(body).get("player") if code == 200 else None
        except ValueError:
            player = None
        if not player:
            self.report({"ERROR"}, "The game is not running on API port %d, or the player is not in a town" % settings.debug_port)
            return {"CANCELLED"}
        context.scene.cursor.location = schema.game_to_blender(player)
        root = c2data.current_map_root(context)
        if root is not None and not on_map(settings, c2data.map_name(root)):
            self.report({"WARNING"}, "The game is on another map than %s" % c2data.map_name(root))
        if context.area is not None and context.area.type == "VIEW_3D":
            bpy.ops.view3d.view_center_cursor()
        return {"FINISHED"}


def draw(layout, context):
    row = layout.row(align=True)
    row.operator(DC2TK_OT_play_in_game.bl_idname, icon="PLAY")
    row.prop(context.window_manager.dc2tk_maps, "play_start", text="")
    row.operator(DC2TK_OT_cursor_to_player.bl_idname, text="", icon="PIVOT_CURSOR")
    if status["text"]:
        layout.label(text=status["text"], icon="INFO")


classes = (DC2TK_OT_play_in_game, DC2TK_OT_cursor_to_player)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
