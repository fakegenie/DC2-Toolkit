"""The map's outliner: its placements, pieces, points and cameras in one list to search and filter,
which selects what is clicked."""

import bpy

from . import c2data, schema

ROLE_ICONS = {
    "placement": "OUTLINER_OB_EMPTY",
    "piece": "OBJECT_DATA",
    "point": "EMPTY_AXIS",
    "camera": "CAMERA_DATA",
}
KIND_ICONS = {
    "pos": "EMPTY_SINGLE_ARROW",
    "event": "EVENT_E",
    "invent": "IMAGE_DATA",
    "plight": "LIGHT_POINT",
    "sound": "SPEAKER",
    "anime": "ANIM",
    "effect": "SHADERFX",
    "fire": "LIGHT_SUN",
    "flare": "LIGHT_SUN",
}

FILTERS = (
    ("PLACEMENTS", "Placements", "The map's placements", "OUTLINER_OB_EMPTY", 0),
    ("POINTS", "Points", "Trigger points: doors, events, lights, sounds...", "EMPTY_AXIS", 1),
    ("PIECES", "Pieces", "The pieces of every placement", "OBJECT_DATA", 2),
    ("CAMERAS", "Cameras", "Fixed cameras", "CAMERA_DATA", 3),
)

_cache = {"key": None, "flags": None}


# bumped whenever the scene changes, for what panels work out from the whole map
generation = {"count": 0}
# bumped when more than where things are changes: what the map holds, its meshes, pictures, data
content = {"count": 0}


def moved_only(depsgraph):
    for update in depsgraph.updates:
        if isinstance(update.id, bpy.types.Scene):
            continue
        if not isinstance(update.id, bpy.types.Object) or update.is_updated_geometry or not update.is_updated_transform:
            return False
    return True


@bpy.app.handlers.persistent
def invalidate(_scene=None, depsgraph=None):
    _cache["key"] = None
    generation["count"] += 1
    if depsgraph is None or not moved_only(depsgraph):
        content["count"] += 1


def describe(obj):
    """What a row says after the name: a placement's template, a point's kind, a piece's model."""
    role = c2data.role(obj)
    if role == "placement":
        text = c2data.template_name(obj)
        if c2data.is_preview(obj):
            return text + " (template)"
        count = len(c2data.instances(obj)) if bpy.context.window_manager.dc2tk_maps.outline_counts else 0
        return "%s ×%d" % (text, count) if count > 1 else text
    if role == "point":
        kind = c2data.lookup(obj, ("kind",), "?")
        if kind == "event":
            return "event %s" % c2data.lookup(obj, ("data", "kind"), "")
        return schema.KIND_LABELS.get(kind, kind)
    if role == "piece":
        return str(c2data.lookup(obj, ("model",), ""))
    return ""


def wanted(obj, settings, root):
    role = c2data.role(obj)
    want = {"PLACEMENTS": "placement", "POINTS": "point", "PIECES": "piece", "CAMERAS": "camera"}[settings.outline_filter]
    if role != want:
        return False
    if root is not None and c2data.map_root(obj) is not root:
        return False
    if want == "point" and settings.outline_kind != "ALL" and c2data.lookup(obj, ("kind",)) != settings.outline_kind:
        return False
    text = settings.outline_search.strip().lower()
    if text:
        name = c2data.exact_name(obj, role).lower()
        if text not in name and text not in describe(obj).lower():
            return False
    if settings.outline_referenced:
        from . import scripts

        if not scripts.references(obj):
            return False
    return True


class DC2TK_UL_map_objects(bpy.types.UIList):
    """The map's objects of one role, found by name, template, kind or model."""

    def draw_item(self, context, layout, data, item, icon, active_data, active_property, index=0, flt_flag=0):
        obj = item
        role = c2data.role(obj)
        icon = ROLE_ICONS.get(role, "OBJECT_DATA")
        if role == "point":
            icon = KIND_ICONS.get(c2data.lookup(obj, ("kind",)), icon)
        row = layout.row(align=True)
        name = c2data.exact_name(obj, role)
        shown = c2data.lookup(obj, ("show",), 1) if role in ("placement", "piece") else c2data.lookup(obj, ("enable",), 1)
        sub = row.row(align=True)
        sub.active = bool(shown) if isinstance(shown, (int, float)) else True
        sub.label(text=name, icon=icon)
        detail = row.row()
        detail.alignment = "RIGHT"
        detail.label(text=describe(obj))
        from . import scripts

        if scripts.references(obj):
            row.label(text="", icon="TEXT")

    def filter_items(self, context, data, propname):
        objects = getattr(data, propname)
        settings = context.window_manager.dc2tk_maps
        root = c2data.current_map_root(context)
        key = (
            len(objects), root.name if root else None, settings.outline_filter, settings.outline_kind,
            settings.outline_search, settings.outline_referenced,
        )
        if _cache["key"] != key or _cache["flags"] is None or len(_cache["flags"]) != len(objects):
            flags = [self.bitflag_filter_item if wanted(obj, settings, root) else 0 for obj in objects]
            _cache["key"], _cache["flags"] = key, flags
        return list(_cache["flags"]), []


def select_listed(settings, context):
    """Selects the object picked in the list and makes it active."""
    index = settings.outline_index
    if not 0 <= index < len(bpy.data.objects):
        return
    obj = bpy.data.objects[index]
    if obj.name not in context.view_layer.objects:
        return
    for other in context.selected_objects:
        other.select_set(False)
    try:
        obj.select_set(True)
    except RuntimeError:
        return
    context.view_layer.objects.active = obj


class DC2TK_OT_frame_listed(bpy.types.Operator):
    """Show the selected object in the 3D view"""

    bl_idname = "dc2tk.frame_listed"
    bl_label = "Frame Selected"

    def execute(self, context):
        for area in context.screen.areas if context.screen else []:
            if area.type != "VIEW_3D":
                continue
            region = next((r for r in area.regions if r.type == "WINDOW"), None)
            if region is None:
                continue
            with context.temp_override(area=area, region=region):
                bpy.ops.view3d.view_selected()
            return {"FINISHED"}
        return {"CANCELLED"}


def draw(layout, context):
    settings = context.window_manager.dc2tk_maps
    row = layout.row(align=True)
    row.prop(settings, "outline_filter", expand=True, icon_only=True)
    row = layout.row(align=True)
    row.prop(settings, "outline_search", text="", icon="VIEWZOOM")
    if settings.outline_filter == "POINTS":
        row.prop(settings, "outline_kind", text="")
    row.prop(settings, "outline_referenced", text="", icon="TEXT")
    layout.template_list("DC2TK_UL_map_objects", "", bpy.data, "objects", settings, "outline_index", rows=6)
    row = layout.row(align=True)
    row.operator(DC2TK_OT_frame_listed.bl_idname, icon="ZOOM_SELECTED")
    row.prop(settings, "outline_counts", text="", icon="LINKED")


classes = (DC2TK_UL_map_objects, DC2TK_OT_frame_listed)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    for handlers in (bpy.app.handlers.depsgraph_update_post, bpy.app.handlers.undo_post, bpy.app.handlers.load_post):
        if invalidate not in handlers:
            handlers.append(invalidate)


def unregister():
    for handlers in (bpy.app.handlers.depsgraph_update_post, bpy.app.handlers.undo_post, bpy.app.handlers.load_post):
        if invalidate in handlers:
            handlers.remove(invalidate)
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
