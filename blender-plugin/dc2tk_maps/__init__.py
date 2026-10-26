bl_info = {
    "name": "DC2 Maps (dc2tk)",
    "author": "DC2-Toolkit",
    "version": (1, 1, 0),
    "blender": (3, 6, 0),
    "location": "File > Import > DC2 map; View3D > Sidebar > DC2; Add > DC2",
    "description": "Edit DC2 (PS2) maps exported by dc2tk, make new ones, and build them into a mod",
    "category": "Import-Export",
}

if "bpy" in locals():
    import importlib

    for _module in _all:
        importlib.reload(_module)

import bpy

from . import (
    schema, c2data, look, lighting, prefs, props, runner, scripts, import_map, edit, tools, outliner, budgets,
    validate, export_mod, testing, play, picking, wizards, library, collision, animate, views, gizmos, overlay, menus,
    ui,
)

_all = (
    schema, c2data, look, lighting, prefs, props, runner, scripts, import_map, edit, tools, outliner, budgets,
    validate, export_mod, testing, play, picking, wizards, library, collision, animate, views, gizmos, overlay, menus,
    ui,
)
_modules = (
    prefs, props, lighting, import_map, edit, tools, outliner, validate, export_mod, testing, play, picking, wizards,
    library, collision, animate, views, gizmos, overlay, menus, ui,
)


def register():
    for module in _modules:
        module.register()


def unregister():
    for module in reversed(_modules):
        module.unregister()
