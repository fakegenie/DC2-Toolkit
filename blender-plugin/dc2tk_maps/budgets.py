"""The Budgets panel: what the map uses of the game's fixed room, from the scene as it is and from
the last Preview Changes (dc2tk build --dry-run, whose memory check is the engine's own sums)."""

import bpy
import numpy

from . import c2data, outliner, schema

PREVIEW_TEXT = "dc2tk preview"


def placement_slots(root):
    """Placements the built map has: every slot the disc's map had (deleted ones are kept hidden,
    since scripts number them) and the new ones after them."""
    placed = [obj for obj in c2data.placements(root) if not c2data.is_preview(obj)]
    slots = [c2data.lookup(obj, ("slot",)) for obj in placed]
    kept = max([slot for slot in slots if isinstance(slot, int)] + [-1]) + 1
    new = sum(1 for slot in slots if not isinstance(slot, int))
    return kept + new, new


def new_models(root):
    """Models made in Blender: pieces whose meshes carry no mds_index (no model of the disc)."""
    children = c2data.child_map()
    found = set()
    for piece in c2data.descendants(root, children):
        if c2data.role(piece) != "piece":
            continue
        meshes = [obj for obj in c2data.descendants(piece, children) if obj.type == "MESH"]
        if meshes and all("mds_index" not in (c2data.get(obj) or {}) for obj in meshes):
            found.add(str(c2data.lookup(piece, ("model",), "")).lower())
    return sorted(found)


def new_pictures(root):
    from . import validate

    found = {}
    for obj in c2data.map_objects(root):
        if obj.type != "MESH" or c2data.is_collision_mesh(obj):
            continue
        for slot in obj.material_slots:
            material = slot.material
            if material is None or material.node_tree is None:
                continue
            for node in material.node_tree.nodes:
                image = getattr(node, "image", None) if node.type == "TEX_IMAGE" else None
                if image is not None and not validate.is_exported_picture(image):
                    found[validate.picture_name(image)] = tuple(image.size[:])
    return found


def mesh_triangles(mesh):
    totals = numpy.zeros(len(mesh.polygons), dtype=numpy.int32)
    mesh.polygons.foreach_get("loop_total", totals)
    return int(totals.sum()) - 2 * len(totals)


def triangles(root):
    drawn = collision = 0
    counted = {}
    for obj in c2data.map_objects(root):
        if obj.type != "MESH":
            continue
        if obj.data not in counted:
            counted[obj.data] = mesh_triangles(obj.data)
        count = counted[obj.data]
        if c2data.is_collision_mesh(obj):
            collision += count
        else:
            drawn += count
    return drawn, collision


def preview_lines(name):
    """The last preview's memory, warning and error lines about this map."""
    text = bpy.data.texts.get(PREVIEW_TEXT)
    if text is None:
        return []
    found = []
    for line in text.as_string().splitlines():
        lowered = line.lower()
        if lowered.startswith(("memory:", "warning:", "error:")) and ("/%s." % name.lower() in lowered or " %s" % name.lower() in lowered):
            found.append(line)
    return found


def meter(layout, label, used, limit, note=""):
    row = layout.row(align=True)
    over = limit and used > limit
    row.alert = bool(over)
    text = "%s: %d / %d" % (label, used, limit) if limit else "%s: %d" % (label, used)
    row.label(text=text + (" %s" % note if note else ""), icon="ERROR" if over else "BLANK1")
    if limit and hasattr(row, "progress"):
        row.progress(factor=min(1.0, used / float(limit)), text="%d%%" % round(100.0 * used / limit))


_figures = {"key": None}


def figures(root):
    key = (root.name, outliner.content["count"])
    if _figures["key"] != key:
        placed = c2data.placements(root)
        _figures.update(key=key, slots=placement_slots(root), models=new_models(root), pictures=new_pictures(root),
                        triangles=triangles(root), placements=len(placed), points=len(c2data.points(root)),
                        templates=any(c2data.is_preview(obj) for obj in placed))
    return _figures


def draw(layout, context, root):
    name = c2data.map_name(root)
    dungeon = name.startswith("d")
    found = figures(root)
    slots, new = found["slots"]
    box = layout.box()
    meter(box, "Placements", slots, schema.PLACEMENT_LIMIT_DUNGEON if dungeon else schema.PLACEMENT_LIMIT,
          "(%d new)" % new if new else "")
    models = found["models"]
    meter(box, "New models", len(models), 0, "(a pack holds %d)" % schema.PACK_MODELS)
    pictures = found["pictures"]
    texels = sum(w * h for w, h in pictures.values())
    meter(box, "New pictures", len(pictures), 0, "(%d texels)" % texels if pictures else "")
    drawn, collision = found["triangles"]
    box.label(text="Triangles: %d drawn, %d collision" % (drawn, collision), icon="MESH_DATA")
    lines = preview_lines(name)
    box = layout.box()
    if not lines:
        box.label(text="Preview Changes for the engine's memory sums", icon="INFO")
        return
    col = box.column(align=True)
    for line in lines[:10]:
        kind, _, rest = line.partition(": ")
        icon = {"error": "ERROR", "warning": "ERROR"}.get(kind.lower(), "MEMORY")
        sub = col.row()
        sub.alert = kind.lower() == "error"
        sub.label(text=rest, icon=icon)
