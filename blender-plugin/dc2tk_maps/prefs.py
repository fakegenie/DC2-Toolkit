import os
import sys

import bpy
from bpy.props import IntProperty, StringProperty

PACKAGE = __package__


class Dc2tkPreferences(bpy.types.AddonPreferences):
    bl_idname = PACKAGE

    repo_dir: StringProperty(
        name="dc2tk repository",
        description="The DC2-Toolkit folder (holding dc2tk.bat, ./dc2tk and extracted/)",
        subtype="DIR_PATH",
        default="",
    )
    dc2tk_path: StringProperty(
        name="dc2tk executable",
        description="dc2tk itself; empty to use build/release/dc2tk(.exe), dc2tk(.exe), dc2tk.bat or ./dc2tk in the repository",
        subtype="FILE_PATH",
        default="",
    )
    export_args: StringProperty(
        name="Map export arguments",
        description="dc2tk's arguments to export a map; {name} is the map, {out} the folder",
        default="map-export {name} -o {out}",
    )
    maps_dir: StringProperty(
        name="Exported maps",
        description="Where dc2tk exports maps to; empty for extracted/maps in the repository",
        subtype="DIR_PATH",
        default="",
    )
    mod_dir: StringProperty(
        name="Mod folder",
        description="The mod the map is exported into (mods/NAME, holding mod.json)",
        subtype="DIR_PATH",
        default="",
    )
    source_dir: StringProperty(
        name="Source tree",
        description="The decompiled scripts dc2tk build compiles; empty for extracted/scripts in the repository",
        subtype="DIR_PATH",
        default="",
    )
    iso_path: StringProperty(
        name="Retail disc",
        description="The game's ISO the build starts from, or an untouched extracted disc folder (extracted/disc)",
        subtype="FILE_PATH",
        default="",
    )
    output_iso: StringProperty(
        name="Output disc image",
        description="Where dc2tk build writes the modded ISO",
        subtype="FILE_PATH",
        default="",
    )
    port_binary: StringProperty(
        name="Debug port",
        description="The native port's debug build Play in Game runs; empty for build/port/dc2-debug in the repository",
        subtype="FILE_PATH",
        default="",
    )
    overrides_dir: StringProperty(
        name="Override folder",
        description="Where Play in Game builds the mod's game files for the port to read; empty for overrides beside the port",
        subtype="DIR_PATH",
        default="",
    )
    debug_port: IntProperty(
        name="API port",
        description="The port of the debug build's API (DC2TK_DEBUG_PORT)",
        default=7777,
        min=1,
        max=65535,
    )

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "repo_dir")
        layout.prop(self, "dc2tk_path")
        found = dc2tk_command(self)
        layout.label(text="dc2tk: " + (" ".join(found) if found else "not found"), icon="CONSOLE")
        layout.prop(self, "export_args")
        layout.prop(self, "maps_dir")
        layout.separator()
        layout.prop(self, "mod_dir")
        layout.prop(self, "source_dir")
        layout.prop(self, "iso_path")
        layout.prop(self, "output_iso")
        layout.separator()
        layout.prop(self, "port_binary")
        layout.prop(self, "overrides_dir")
        layout.prop(self, "debug_port")


def get(context=None):
    context = context or bpy.context
    return context.preferences.addons[PACKAGE].preferences


def absolute(path):
    return os.path.abspath(bpy.path.abspath(path)) if path else ""


def _own_repo():
    here = os.path.dirname(os.path.realpath(__file__))
    root = os.path.dirname(os.path.dirname(here))
    if os.path.isfile(os.path.join(root, "dc2tk.bat")) and os.path.isfile(os.path.join(root, "src", "Cargo.toml")):
        return root
    return ""


def repo(prefs):
    return absolute(prefs.repo_dir) if prefs.repo_dir else _own_repo()


def dc2tk_command(prefs):
    if prefs.dc2tk_path:
        path = absolute(prefs.dc2tk_path)
        return [path] if os.path.exists(path) else None
    root = repo(prefs)
    if not root:
        return None
    windows = sys.platform.startswith("win")
    candidates = [os.path.join(root, "build", "release", "dc2tk.exe" if windows else "dc2tk")]
    # a release's folder has the executable at its top; a checkout has the scripts that build it
    candidates.append(os.path.join(root, "dc2tk.exe" if windows else "dc2tk"))
    if windows:
        candidates.append(os.path.join(root, "dc2tk.bat"))
    for candidate in candidates:
        if os.path.isfile(candidate):
            return [candidate]
    return None


def maps_dir(prefs):
    if prefs.maps_dir:
        return absolute(prefs.maps_dir)
    root = repo(prefs)
    return os.path.join(root, "extracted", "maps") if root else ""


def source_dir(prefs):
    if prefs.source_dir:
        return absolute(prefs.source_dir)
    root = repo(prefs)
    return os.path.join(root, "extracted", "scripts") if root else ""


def working_dir(prefs):
    root = repo(prefs)
    if root and os.path.isdir(root):
        return root
    command = dc2tk_command(prefs)
    return os.path.dirname(command[0]) if command else os.getcwd()


classes = (Dc2tkPreferences,)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
