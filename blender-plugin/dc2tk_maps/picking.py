import bpy
from bpy.app.handlers import persistent

from . import c2data

OWNER = object()
KEEP = {"placement", "point", "camera", "group", "map", "display"}
_busy = {"on": False}


def target(obj, level):
    if obj is None or level == "OBJECT" or c2data.role(obj) in KEEP:
        return None
    found = c2data.placement_of(obj) if level == "PLACEMENT" else c2data.piece_of(obj)
    if found is None and level == "PIECE":
        found = c2data.placement_of(obj)
    return found if found is not obj else None


def redirect(context):
    settings = getattr(context.window_manager, "dc2tk_maps", None)
    if settings is None or context.mode != "OBJECT" or _busy["on"]:
        return
    level = settings.select_level
    active = context.view_layer.objects.active
    chosen = target(active, level)
    others = [(obj, target(obj, level)) for obj in context.selected_objects if obj is not active]
    if chosen is None and not any(found for _, found in others):
        return
    _busy["on"] = True
    try:
        for obj, found in others:
            if found is not None:
                obj.select_set(False)
                found.select_set(True)
        if chosen is not None:
            active.select_set(False)
            chosen.select_set(True)
            context.view_layer.objects.active = chosen
    finally:
        _busy["on"] = False


def active_changed():
    redirect(bpy.context)


def subscribe():
    bpy.msgbus.clear_by_owner(OWNER)
    bpy.msgbus.subscribe_rna(key=(bpy.types.LayerObjects, "active"), owner=OWNER, args=(), notify=active_changed)


@persistent
def after_load(_scene=None):
    subscribe()


def register():
    if bpy.app.background:
        return
    subscribe()
    if after_load not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(after_load)


def unregister():
    bpy.msgbus.clear_by_owner(OWNER)
    if after_load in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(after_load)
