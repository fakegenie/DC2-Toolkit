import bpy
from bpy.app.handlers import persistent

from . import c2data, schema

_handle = None

_batches = None

EDGES = ((0, 1), (1, 3), (3, 2), (2, 0), (4, 5), (5, 7), (7, 6), (6, 4), (0, 4), (1, 5), (2, 6), (3, 7))


def _shader():
    import gpu

    for name in ("UNIFORM_COLOR", "3D_UNIFORM_COLOR"):
        try:
            return gpu.shader.from_builtin(name)
        except (ValueError, TypeError):
            continue
    return None


def box_lines(obj):
    fields = schema.DATA_FIELDS["invent"]
    low = [float(v) for v in c2data.data_field(obj, fields[1])]
    high = [float(v) for v in c2data.data_field(obj, fields[2])]
    corners = []
    for x in (low[0], high[0]):
        for y in (low[1], high[1]):
            for z in (low[2], high[2]):
                corners.append(obj.matrix_world @ _vector(schema.game_to_blender((x, y, z))))
    lines = []
    for a, b in EDGES:
        lines.append(corners[a][:])
        lines.append(corners[b][:])
    return lines


def _vector(values):
    from mathutils import Vector

    return Vector(values)


def box_coords(context):
    lines = []
    selected = []
    for obj in context.visible_objects:
        if obj.type != "EMPTY" or c2data.KEY not in obj:
            continue
        if c2data.lookup(obj, ("kind",)) != "invent":
            continue
        try:
            found = box_lines(obj)
        except (TypeError, ValueError, IndexError):
            continue
        (selected if obj.select_get() else lines).extend(found)
    return lines, selected


ARC_RADIUS = 20.0
AXIS_HALF = 30.0
LOOP_FRAMES = 50


def _anime_values(obj):
    fields = {field["key"]: field for field in schema.DATA_FIELDS["anime"]}

    def number(key):
        try:
            return int(c2data.data_field(obj, fields[key]))
        except (TypeError, ValueError):
            return 0

    vectors = {key: [float(v) for v in c2data.data_field(obj, fields[key])] for key in ("start", "step", "end")}
    return number("drives"), number("mode"), number("relative"), vectors


def _axis_index(mode, vectors):
    if mode in (8, 9):
        return 1
    if mode in (1, 2, 3, 4, 5):
        change = vectors["step"]
    elif mode in (6, 7, 10):
        change = [b - a for a, b in zip(vectors["start"], vectors["end"])]
    else:
        return None
    index = max(range(3), key=lambda i: abs(change[i]))
    return index if abs(change[index]) > 1e-9 else None


def anime_lines(obj, by_name=None, link=False, nodes_of=None):
    import math

    from mathutils import Euler, Matrix, Vector

    target, _problem = c2data.anime_target(obj, by_name, nodes_of)
    if target is None:
        return []
    drives, mode, relative, vectors = _anime_values(obj)
    parent = target.parent.matrix_world if target.parent is not None else Matrix.Identity(4)
    centre = target.matrix_world.translation.copy()
    lines = []
    if link:
        lines += [obj.matrix_world.translation[:], centre[:]]
    if drives == 1 and not relative:
        start = vectors["start"]
        if mode == 1:
            end = [s + d * LOOP_FRAMES for s, d in zip(start, vectors["step"])]
        elif mode in (2, 3, 4, 5, 7, 10):
            end = vectors["end"]
        else:
            return lines
        lines += [(parent @ Vector(schema.game_to_blender(v)))[:] for v in (start, end)]
    elif drives == 2:
        index = _axis_index(mode, vectors)
        if index is None:
            return lines
        start = [math.radians(v) for v in vectors["start"]]
        unit = [Vector((1, 0, 0)), Vector((0, 1, 0)), Vector((0, 0, 1))]

        kept = [0.0, start[1] if index == 0 else 0.0, start[2] if index < 2 else 0.0]
        axis_game = Euler(kept, "XYZ").to_matrix() @ unit[index]
        turn = parent.to_3x3()
        axis = (turn @ Vector(schema.game_to_blender(axis_game))).normalized()
        if axis.length < 0.5:
            return lines
        lines += [(centre - axis * AXIS_HALF)[:], (centre + axis * AXIS_HALF)[:]]
        facing = Euler(start, "XYZ").to_matrix() @ unit[0 if index == 2 else 2]
        across = turn @ Vector(schema.game_to_blender(facing))
        across -= axis * across.dot(axis)
        if across.length < 1e-6:
            across = axis.orthogonal()
        across.normalize()
        if mode in (1, 8, 9):
            sweep = 360.0
        else:
            sweep = vectors["end"][index] - vectors["start"][index]
            sweep = max(-360.0, min(360.0, sweep))
        if turn.determinant() < 0:
            sweep = -sweep
        steps = max(4, int(abs(sweep) / 10.0))
        arc = [
            centre + (Matrix.Rotation(math.radians(sweep * i / steps), 3, axis) @ across) * ARC_RADIUS
            for i in range(steps + 1)
        ]
        lines += [centre[:], arc[0][:]]
        for a, b in zip(arc, arc[1:]):
            lines += [a[:], b[:]]
    return lines


def anime_coords(context):
    lines = []
    selected = []
    by_root = {}
    nodes_of = {}
    for obj in context.visible_objects:
        if obj.type != "EMPTY" or c2data.KEY not in obj:
            continue
        if c2data.lookup(obj, ("kind",)) != "anime":
            continue
        root = c2data.map_root(obj)
        if root not in by_root:
            by_root[root] = c2data.placements_by_name(root)
        chosen = obj.select_get()
        try:
            found = anime_lines(obj, by_root[root], link=chosen, nodes_of=nodes_of)
        except (TypeError, ValueError, IndexError):
            continue
        (selected if chosen else lines).extend(found)
    return lines, selected


def _game_matrix(pos, rot, scale):
    """A game transform T·Rz·Ry·Rx·S (radians) as a Blender matrix."""
    from mathutils import Euler, Matrix

    axes = Matrix(((1.0, 0.0, 0.0, 0.0), (0.0, 0.0, -1.0, 0.0), (0.0, 1.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0)))
    game = Matrix.Translation(pos) @ Euler(rot, "XYZ").to_matrix().to_4x4() @ Matrix.Diagonal(list(scale) + [1.0])
    return axes @ game @ axes.inverted()


def _numbers(values, size, default=0.0):
    values = c2data.to_py(values)
    if not isinstance(values, list):
        values = []
    out = []
    for value in values[:size]:
        try:
            out.append(float(value))
        except (TypeError, ValueError):
            out.append(default)
    return out + [default] * (size - len(out))


def _box(matrix, low, high):
    corners = [matrix @ _vector(schema.game_to_blender((x, y, z))) for x in (low[0], high[0]) for y in (low[1], high[1]) for z in (low[2], high[2])]
    lines = []
    for a, b in EDGES:
        lines += [corners[a][:], corners[b][:]]
    return lines


def camera_coords(context):
    """The fixed cameras' boxes (FIX_CAMERA_RECT, in their own transform) and lines from each
    camera to its other positions (FIX_CAMERA_POS2)."""
    from mathutils import Matrix

    lines, selected = [], []
    for root in c2data.scene_map_roots(context.scene):
        cameras = c2data.to_py(c2data.lookup(root, ("map", "cameras")))
        if not isinstance(cameras, list):
            continue
        chosen = {c2data.lookup(obj, ("index",)) for obj in context.selected_objects if c2data.role(obj) == "camera"}
        base = root.matrix_world
        for index, camera in enumerate(cameras):
            if not isinstance(camera, dict):
                continue
            out = selected if index in chosen else lines
            at = base @ _vector(schema.game_to_blender(_numbers(camera.get("pos"), 3)))
            for other in camera.get("pos2") or []:
                if isinstance(other, dict):
                    to = base @ _vector(schema.game_to_blender(_numbers(other.get("pos"), 3)))
                    out += [at[:], to[:]]
            for rect in camera.get("rects") or []:
                if not isinstance(rect, dict):
                    continue
                matrix = base @ _game_matrix(
                    _numbers(rect.get("pos"), 3), _numbers(rect.get("rot"), 3), _numbers(rect.get("scale"), 3, 1.0)
                ) if rect.get("pos") is not None else base @ Matrix.Identity(4)
                out += _box(matrix, _numbers(rect.get("min"), 3), _numbers(rect.get("max"), 3))
    return lines, selected


def cell_coords(context):
    """Each template's dungeon cell (320 x 320 about its origin) on the ground, with its closed
    sides (MOVE_FLAG -X, -Z, +X, +Z) drawn as walls."""
    half = schema.DUNGEON_CELL / 2.0
    lines, walls = [], []
    for obj in context.visible_objects:
        if c2data.role(obj) != "placement" or not c2data.is_preview(obj):
            continue
        world = obj.matrix_world
        corner = lambda x, z, y=0.0: (world @ _vector(schema.game_to_blender((x, y, z))))[:]
        sides = (((-half, -half), (-half, half)), ((-half, -half), (half, -half)), ((half, -half), (half, half)), ((-half, half), (half, half)))
        flags = _numbers(c2data.lookup(obj, ("move_flag",)), 4)
        for side, ((x0, z0), (x1, z1)) in enumerate(sides):
            lines += [corner(x0, z0), corner(x1, z1)]
            if flags[side]:
                for height in (40.0, 80.0):
                    walls += [corner(x0, z0, height), corner(x1, z1, height)]
                walls += [corner(x0, z0), corner(x0, z0, 80.0), corner(x1, z1), corner(x1, z1, 80.0)]
    return lines, walls


def spot_coords(context):
    """Where the map's scripts place the player (Read Map Scripts): a cross on the ground and an
    arrow the way the player faces."""
    import math

    from . import scripts

    lines, selected = [], []
    for root in c2data.scene_map_roots(context.scene):
        report = scripts.info(root) or {}
        base = root.matrix_world
        for spot in report.get("player_positions") or []:
            try:
                x, y, z = float(spot["x"]), float(spot["y"]), float(spot["z"])
            except (KeyError, TypeError, ValueError):
                continue
            yaw = spot.get("yaw")
            at = lambda dx, dz, dy=0.0: (base @ _vector(schema.game_to_blender((x + dx, y + dy, z + dz))))[:]
            lines += [at(-8, 0), at(8, 0), at(0, -8), at(0, 8), at(0, 0), at(0, 0, 40)]
            if isinstance(yaw, (int, float)):
                dx, dz = 25.0 * math.sin(yaw), 25.0 * math.cos(yaw)
                lines += [at(0, 0, 20), at(dx, dz, 20)]
    return lines, selected


def cfg_coords(context):
    """What the map's .cfg adds (exported into the root's cfg): water surfaces, occlusion planes,
    the areas drawn without their parts, extra trigger points and where villagers stand."""
    import math

    lines, selected = [], []
    for root in c2data.scene_map_roots(context.scene):
        cfg = c2data.to_py(c2data.lookup(root, ("cfg",)))
        if not isinstance(cfg, dict):
            continue
        base = root.matrix_world
        at = lambda point: (base @ _vector(schema.game_to_blender(point)))[:]
        for water in cfg.get("water") or []:
            low, high = _numbers(water.get("corner0"), 3), _numbers(water.get("corner1"), 3)
            pos = _numbers(water.get("pos"), 3)
            corners = [(x + pos[0], low[1] + pos[1], z + pos[2]) for x, z in ((low[0], low[2]), (high[0], low[2]), (high[0], high[2]), (low[0], high[2]))]
            for a, b in zip(corners, corners[1:] + corners[:1]):
                lines += [at(a), at(b)]
            lines += [at(corners[0]), at(corners[2]), at(corners[1]), at(corners[3])]
        for plane in cfg.get("occlusion") or []:
            corners = [_numbers(corner, 3) for corner in plane[:4]] if isinstance(plane, list) else []
            for a, b in zip(corners, corners[1:] + corners[:1]):
                lines += [at(a), at(b)]
        for area in cfg.get("draw_off") or []:
            for key in ("camera", "hidden"):
                pair = area.get(key) if isinstance(area, dict) else None
                if isinstance(pair, list) and len(pair) == 2:
                    lines += _box(base, _numbers(pair[0], 3), _numbers(pair[1], 3))
        for event in cfg.get("events") or []:
            pos = _numbers(event.get("pos"), 9)
            matrix = base @ _game_matrix(pos[0:3], pos[3:6], [1.0, 1.0, 1.0])
            half = [abs(s) or 1.0 for s in pos[6:9]]
            lines += _box(matrix, [-h for h in half], half)
        for villager in cfg.get("villagers") or []:
            x, y, z = _numbers(villager.get("pos"), 3)
            yaw = float(villager.get("yaw") or 0.0)
            lines += [at((x - 6, y, z)), at((x + 6, y, z)), at((x, y, z - 6)), at((x, y, z + 6)), at((x, y, z)), at((x, y + 30, z))]
            lines += [at((x, y + 15, z)), at((x + 20 * math.sin(yaw), y + 15, z + 20 * math.cos(yaw)))]
    return lines, selected


def walkable_tris(context):
    """The shown floor collision's triangles by what the player does on them: stand (the
    triangle faces up at least as much as the engine's slope limit), meet a wall, or swim (areas
    1 and 7)."""
    floor, wall, water = [], [], []
    for root in c2data.scene_map_roots(context.scene):
        for obj in c2data.collision_meshes(root):
            if c2data.lookup(c2data.piece_of(obj), ("kind",)) != "collision" or not obj.visible_get():
                continue
            mesh = obj.data
            world = obj.matrix_world
            turn = world.to_3x3()
            names = [slot.material.name if slot.material else "" for slot in obj.material_slots]
            for polygon in mesh.polygons:
                if len(polygon.vertices) < 3:
                    continue
                normal = (turn @ polygon.normal).normalized()
                attrs = schema.parse_col(names[polygon.material_index]) if polygon.material_index < len(names) else None
                if attrs is not None and attrs[2] in schema.WATER_AREAS:
                    out = water
                elif normal.z >= schema.WALL_NORMAL_Y:
                    out = floor
                else:
                    out = wall
                corners = [world @ mesh.vertices[index].co for index in polygon.vertices]
                for i in range(1, len(corners) - 1):
                    out += [corners[0][:], corners[i][:], corners[i + 1][:]]
    return [(floor, (0.2, 0.9, 0.3, 0.35)), (wall, (0.95, 0.25, 0.2, 0.4)), (water, (0.2, 0.45, 1.0, 0.4))]


def invalidate(*_args):
    global _batches
    _batches = None


@persistent
def _scene_changed(_scene, _depsgraph=None):
    invalidate()


SELECTED = (1.0, 0.7, 0.2, 1.0)
LAYERS = (
    ("show_boxes", box_coords, (0.3, 0.9, 0.9, 1.0)),
    ("show_anime", anime_coords, (0.85, 0.45, 0.95, 1.0)),
    ("show_camera_boxes", camera_coords, (0.95, 0.85, 0.3, 1.0)),
    ("show_spots", spot_coords, (1.0, 0.4, 0.7, 1.0)),
    ("show_cfg", cfg_coords, (0.45, 0.75, 1.0, 1.0)),
)
# cells and walls are drawn as plain lines and as walls of the closed sides
CELL_COLOURS = ((0.6, 0.6, 0.6, 1.0), (1.0, 0.35, 0.2, 1.0))


def _make_batches(context, shader, shown):
    from gpu_extras.batch import batch_for_shader

    batches = []
    for setting, coords_of, colour in LAYERS:
        if setting not in shown:
            continue
        lines, selected = coords_of(context)
        for coords, tint in ((lines, colour), (selected, SELECTED)):
            if coords:
                batches.append((batch_for_shader(shader, "LINES", {"pos": coords}), tint, setting, False))
    if "show_cells" in shown:
        for coords, tint in zip(cell_coords(context), CELL_COLOURS):
            if coords:
                batches.append((batch_for_shader(shader, "LINES", {"pos": coords}), tint, "show_cells", False))
    if "show_walkable" in shown:
        for coords, tint in walkable_tris(context):
            if coords:
                batches.append((batch_for_shader(shader, "TRIS", {"pos": coords}), tint, "show_walkable", True))
    return batches


def draw():
    global _batches
    context = bpy.context
    wm = getattr(context, "window_manager", None)
    if wm is None or not hasattr(wm, "dc2tk_maps"):
        return
    names = [setting for setting, _coords_of, _colour in LAYERS] + ["show_cells", "show_walkable"]
    shown = {setting for setting in names if getattr(wm.dc2tk_maps, setting)}
    if not shown:
        return
    shader = _shader()
    if shader is None:
        return
    if _batches is None or _batches[0] != shown:
        _batches = (shown, _make_batches(context, shader, shown))
    if not _batches[1]:
        return
    import gpu

    gpu.state.line_width_set(1.5)
    shader.bind()
    for batch, colour, setting, filled in _batches[1]:
        if filled:
            gpu.state.blend_set("ALPHA")
            gpu.state.depth_test_set("LESS_EQUAL")
            gpu.state.depth_mask_set(False)
        shader.uniform_float("color", colour)
        batch.draw(shader)
        if filled:
            gpu.state.blend_set("NONE")
            gpu.state.depth_test_set("NONE")
            gpu.state.depth_mask_set(True)
    gpu.state.line_width_set(1.0)


def register():
    global _handle
    if bpy.app.background:
        return
    _handle = bpy.types.SpaceView3D.draw_handler_add(draw, (), "WINDOW", "POST_VIEW")
    for handlers in (bpy.app.handlers.depsgraph_update_post, bpy.app.handlers.undo_post,
                     bpy.app.handlers.redo_post, bpy.app.handlers.load_post):
        if _scene_changed not in handlers:
            handlers.append(_scene_changed)


def unregister():
    global _handle
    if _handle is not None:
        bpy.types.SpaceView3D.draw_handler_remove(_handle, "WINDOW")
        _handle = None
    for handlers in (bpy.app.handlers.depsgraph_update_post, bpy.app.handlers.undo_post,
                     bpy.app.handlers.redo_post, bpy.app.handlers.load_post):
        if _scene_changed in handlers:
            handlers.remove(_scene_changed)
    invalidate()
