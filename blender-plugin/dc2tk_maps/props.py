import bpy
from bpy.props import (
    BoolProperty,
    CollectionProperty,
    EnumProperty,
    FloatProperty,
    FloatVectorProperty,
    IntProperty,
    IntVectorProperty,
    PointerProperty,
    StringProperty,
)

from . import c2data, look, schema


def _active():
    return getattr(bpy.context, "active_object", None)


def _targets(same_kind=False):
    """What a field's edit changes: the active object, and with "All selected" on every selected
    object of its role (and, for a point's data, of its kind), as Alt does for Blender's own
    fields."""
    active = _active()
    if active is None:
        return []
    found = [active]
    wm = getattr(bpy.context, "window_manager", None)
    if wm is None or not getattr(wm.dc2tk_maps, "edit_selected", False):
        return found
    role = c2data.role(active)
    kind = c2data.lookup(active, ("kind",)) if role == "point" else None
    for obj in getattr(bpy.context, "selected_objects", []):
        if obj is active or c2data.role(obj) != role or c2data.is_stringified(obj):
            continue
        if same_kind and c2data.lookup(obj, ("kind",)) != kind:
            continue
        found.append(obj)
    return found


def _each(change, *args):
    # a point's data fields belong to its kind; its hours, flags and kind to every point
    for obj in _targets(same_kind=change is set_point_value):
        change(obj, *args)


def _int(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _float(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def set_value(obj, path, value):
    if obj is not None:
        c2data.assign(obj, path, value)


def set_flag(obj, key, value):
    if obj is None:
        return
    old = c2data.lookup(obj, (key,))
    value = bool(value) if isinstance(old, bool) else int(value)
    c2data.assign(obj, (key,), value)
    if key == "show":
        settings = bpy.context.window_manager.dc2tk_maps
        look.show_hour(bpy.context, settings.hour, settings.by_hour)


def get_pair(obj, key, start):
    values = c2data.to_py(c2data.lookup(obj, (key,), []))
    if not isinstance(values, list):
        values = []
    values += [0] * max(0, start + 2 - len(values))
    return (_int(values[start]), _int(values[start + 1]))


def set_pair(obj, key, start, value):
    if obj is None:
        return
    values = c2data.to_py(c2data.lookup(obj, (key,), []))
    if not isinstance(values, list):
        values = []
    values += [0] * max(0, start + 2 - len(values))
    values[start:start + 2] = [int(value[0]), int(value[1])]
    c2data.assign(obj, (key,), values)


def get_far_clip(obj, index):
    value = c2data.to_py(c2data.lookup(obj, ("far_clip",)))
    if isinstance(value, list) and index < len(value):
        return _float(value[index])
    return 0.0


def set_far_clip(obj, index, value):
    if obj is None:
        return
    current = c2data.to_py(c2data.lookup(obj, ("far_clip",)))
    current = current if isinstance(current, list) else []
    current += [0.0] * max(0, index + 1 - len(current))
    current[index] = value
    c2data.assign(obj, ("far_clip",), current)


def set_collision_type(obj, value):
    if obj is None:
        return
    current = c2data.to_py(c2data.lookup(obj, ("col_type",)))
    if isinstance(current, list) and current:
        current[0] = value
    else:
        current = value
    c2data.assign(obj, ("col_type",), current)


def set_point_kind(obj, value):
    if obj is None:
        return
    kind = schema.POINT_KINDS[value]
    if c2data.lookup(obj, ("kind",)) == kind:
        return
    data = c2data.to_py(c2data.lookup(obj, ("data",)))
    fresh = schema.default_data(kind)
    if isinstance(data, dict):
        fresh.update({key: value for key, value in data.items() if key in fresh})
    c2data.assign(obj, ("kind",), kind)
    c2data.assign(obj, ("data",), fresh)


def get_point_value(obj, kind, key):
    field = schema.data_field(kind, key)
    value = c2data.data_field(obj, field)
    if field["kind"] == "str":
        return str(value)
    if field["choices"]:
        choices = [choice[0] for choice in field["choices"]]
        value = _int(value)
        return value if value in choices else choices[0]
    if field["kind"] == "flag":
        return bool(_int(value))
    if field["size"] > 1:
        if field["kind"] == "int":
            return tuple(_int(item) for item in value)
        return tuple(_float(item) for item in value)
    return _int(value) if field["kind"] == "int" else _float(value)


def set_point_value(obj, kind, key, value):
    if obj is None:
        return
    field = schema.data_field(kind, key)
    if field["kind"] == "flag":
        value = int(value)
    c2data.set_data_field(obj, field, value)
    if kind == "plight" and key == "color" and obj.type == "LIGHT":
        obj.data.color = [max(0.0, min(1.0, _float(c) / 255.0)) for c in value]


def get_node_name(self):
    obj = _active()
    return c2data.exact_name(obj, c2data.role(obj)) if obj is not None else ""


def set_node_name(self, value):
    c2data.rename(_active(), value)


def get_collision_type(self):
    value = c2data.to_py(c2data.lookup(_active(), ("col_type",)))
    if isinstance(value, list):
        return _int(value[0]) if value else 0
    return _int(value)


def set_active_collision_type(self, value):
    _each(set_collision_type, value)


def get_kind(self):
    kind = c2data.lookup(_active(), ("kind",), "pos")
    return schema.POINT_KINDS.index(kind) if kind in schema.POINT_KINDS else 0


def set_kind(self, value):
    _each(set_point_kind, value)


def get_template(self):
    return str(c2data.lookup(_active(), ("template",), ""))


def set_template(self, value):
    _each(set_value, ("template",), value)


def get_group(self):
    return str(c2data.lookup(_active(), ("group",), ""))


def set_group(self, value):
    _each(set_value, ("group",), value)


def get_model(self):
    return str(c2data.lookup(_active(), ("model",), ""))


def set_model(self, value):
    _each(set_value, ("model",), value)


def get_show(self):
    return bool(_int(c2data.lookup(_active(), ("show",), 0)))


def set_show(self, value):
    _each(set_flag, "show", value)


def get_enable(self):
    return bool(_int(c2data.lookup(_active(), ("enable",), 0)))


def set_enable(self, value):
    _each(set_flag, "enable", value)


def get_piece_hours(self):
    return get_pair(_active(), "time", 0)


def set_piece_hours(self, value):
    _each(set_pair, "time", 0, value)


def get_point_flags(self):
    return get_pair(_active(), "flag", 0)


def set_point_flags(self, value):
    _each(set_pair, "flag", 0, value)


def get_point_hours(self):
    return get_pair(_active(), "flag", 2)


def set_point_hours(self, value):
    _each(set_pair, "flag", 2, value)


def get_far_distance(self):
    return get_far_clip(_active(), 0)


def set_far_distance(self, value):
    _each(set_far_clip, 0, value)


def get_far_fade(self):
    return get_far_clip(_active(), 1)


def set_far_fade(self, value):
    _each(set_far_clip, 1, value)


def get_invent_id(self):
    return get_point_value(_active(), "invent", "id")


def set_invent_id(self, value):
    _each(set_point_value, "invent", "id", value)


def get_invent_min(self):
    return get_point_value(_active(), "invent", "min")


def set_invent_min(self, value):
    _each(set_point_value, "invent", "min", value)


def get_invent_max(self):
    return get_point_value(_active(), "invent", "max")


def set_invent_max(self, value):
    _each(set_point_value, "invent", "max", value)


def get_invent_unknown_int(self):
    return get_point_value(_active(), "invent", "unknown_int")


def set_invent_unknown_int(self, value):
    _each(set_point_value, "invent", "unknown_int", value)


def get_invent_unknown_float(self):
    return get_point_value(_active(), "invent", "unknown_float")


def set_invent_unknown_float(self, value):
    _each(set_point_value, "invent", "unknown_float", value)


def get_invent_angle(self):
    return get_point_value(_active(), "invent", "angle")


def set_invent_angle(self, value):
    _each(set_point_value, "invent", "angle", value)


def get_event_kind(self):
    return get_point_value(_active(), "event", "kind")


def set_event_kind(self, value):
    _each(set_point_value, "event", "kind", value)


def get_event_event_no(self):
    return get_point_value(_active(), "event", "event_no")


def set_event_event_no(self, value):
    _each(set_point_value, "event", "event_no", value)


def get_event_program(self):
    return get_point_value(_active(), "event", "program")


def set_event_program(self, value):
    _each(set_point_value, "event", "program", value)


def get_event_args(self):
    return get_point_value(_active(), "event", "args")


def set_event_args(self, value):
    _each(set_point_value, "event", "args", value)


def get_event_target(self):
    return get_point_value(_active(), "event", "target")


def set_event_target(self, value):
    _each(set_point_value, "event", "target", value)


def get_event_flags(self):
    return get_point_value(_active(), "event", "flags")


def set_event_flags(self, value):
    _each(set_point_value, "event", "flags", value)


def get_flare_color(self):
    return get_point_value(_active(), "flare", "color")


def set_flare_color(self, value):
    _each(set_point_value, "flare", "color", value)


def get_flare_draw(self):
    return get_point_value(_active(), "flare", "draw")


def set_flare_draw(self, value):
    _each(set_point_value, "flare", "draw", value)


def get_flare_extra(self):
    return get_point_value(_active(), "flare", "extra")


def set_flare_extra(self, value):
    _each(set_point_value, "flare", "extra", value)


def get_fire_color(self):
    return get_point_value(_active(), "fire", "color")


def set_fire_color(self, value):
    _each(set_point_value, "fire", "color", value)


def get_fire_draw(self):
    return get_point_value(_active(), "fire", "draw")


def set_fire_draw(self, value):
    _each(set_point_value, "fire", "draw", value)


def get_fire_extra(self):
    return get_point_value(_active(), "fire", "extra")


def set_fire_extra(self, value):
    _each(set_point_value, "fire", "extra", value)


def get_plight_power(self):
    return get_point_value(_active(), "plight", "power")


def set_plight_power(self, value):
    _each(set_point_value, "plight", "power", value)


def get_plight_color(self):
    return get_point_value(_active(), "plight", "color")


def set_plight_color(self, value):
    _each(set_point_value, "plight", "color", value)


def get_plight_mode(self):
    return get_point_value(_active(), "plight", "mode")


def set_plight_mode(self, value):
    _each(set_point_value, "plight", "mode", value)


def get_plight_args(self):
    return get_point_value(_active(), "plight", "args")


def set_plight_args(self, value):
    _each(set_point_value, "plight", "args", value)


def get_plight_flicker(self):
    return get_point_value(_active(), "plight", "flicker")


def set_plight_flicker(self, value):
    _each(set_point_value, "plight", "flicker", value)


def get_plight_depth(self):
    return get_point_value(_active(), "plight", "depth")


def set_plight_depth(self, value):
    _each(set_point_value, "plight", "depth", value)


def get_plight_period(self):
    return get_point_value(_active(), "plight", "period")


def set_plight_period(self, value):
    _each(set_point_value, "plight", "period", value)


def get_effect_effect(self):
    return get_point_value(_active(), "effect", "effect")


def set_effect_effect(self, value):
    _each(set_point_value, "effect", "effect", value)


def get_sound_number(self):
    return get_point_value(_active(), "sound", "number")


def set_sound_number(self, value):
    _each(set_point_value, "sound", "number", value)


def get_sound_near(self):
    return get_point_value(_active(), "sound", "near")


def set_sound_near(self, value):
    _each(set_point_value, "sound", "near", value)


def get_sound_far(self):
    return get_point_value(_active(), "sound", "far")


def set_sound_far(self, value):
    _each(set_point_value, "sound", "far", value)


def get_sound_unknown_int(self):
    return get_point_value(_active(), "sound", "unknown_int")


def set_sound_unknown_int(self, value):
    _each(set_point_value, "sound", "unknown_int", value)


def get_sound_shape(self):
    return get_point_value(_active(), "sound", "shape")


def set_sound_shape(self, value):
    _each(set_point_value, "sound", "shape", value)


def get_sound_start(self):
    return get_point_value(_active(), "sound", "start")


def set_sound_start(self, value):
    _each(set_point_value, "sound", "start", value)


def get_sound_end(self):
    return get_point_value(_active(), "sound", "end")


def set_sound_end(self, value):
    _each(set_point_value, "sound", "end", value)


def get_anime_placement(self):
    return get_point_value(_active(), "anime", "placement")


def set_anime_placement(self, value):
    _each(set_point_value, "anime", "placement", value)


def get_anime_piece(self):
    return get_point_value(_active(), "anime", "piece")


def set_anime_piece(self, value):
    _each(set_point_value, "anime", "piece", value)


def get_anime_frame(self):
    return get_point_value(_active(), "anime", "frame")


def set_anime_frame(self, value):
    _each(set_point_value, "anime", "frame", value)


def get_anime_drives(self):
    return get_point_value(_active(), "anime", "drives")


def set_anime_drives(self, value):
    _each(set_point_value, "anime", "drives", value)


def get_anime_mode(self):
    return get_point_value(_active(), "anime", "mode")


def set_anime_mode(self, value):
    _each(set_point_value, "anime", "mode", value)


def get_anime_start(self):
    return get_point_value(_active(), "anime", "start")


def set_anime_start(self, value):
    _each(set_point_value, "anime", "start", value)


def get_anime_step(self):
    return get_point_value(_active(), "anime", "step")


def set_anime_step(self, value):
    _each(set_point_value, "anime", "step", value)


def get_anime_end(self):
    return get_point_value(_active(), "anime", "end")


def set_anime_end(self, value):
    _each(set_point_value, "anime", "end", value)


def get_anime_uniform(self):
    return get_point_value(_active(), "anime", "uniform")


def set_anime_uniform(self, value):
    _each(set_point_value, "anime", "uniform", value)


def get_anime_relative(self):
    return get_point_value(_active(), "anime", "relative")


def set_anime_relative(self, value):
    _each(set_point_value, "anime", "relative", value)


def data_prop_name(kind, field):
    return "d_%s_%s" % (kind, field["key"])


def _show_hour(self, context):
    look.show_hour(context, self.hour, self.by_hour)
    look.update_time_of_day(context)


def _outline_filters():
    from . import outliner

    return outliner.FILTERS


def _select_listed(self, context):
    from . import outliner

    outliner.select_listed(self, context)


def _redraw_overlay(self, context):
    from . import overlay

    overlay.invalidate()
    for area in context.screen.areas if context.screen else []:
        if area.type == "VIEW_3D":
            area.tag_redraw()


def _show_fog(self, context):
    look.update_time_of_day(context)


class Dc2tkMapEntry(bpy.types.PropertyGroup):
    """A map of the disc, as dc2tk map-export --list gives it: name is what the list shows
    ("m01  Palm Brinks"), code the map's own name (m01)."""

    code: StringProperty()
    title: StringProperty()


class Dc2tkSettings(bpy.types.PropertyGroup):

    import_name: StringProperty(
        name="Map",
        description="The map to export with dc2tk and import: pick it from the list, or type its name (s17, d01f01, ...)",
    )
    maps: CollectionProperty(type=Dc2tkMapEntry, description="The disc's maps, from dc2tk map-export --list")
    collision_colour: EnumProperty(
        name="Colour collision by",
        items=(
            ("AREA", "Area", "Area kind (1 and 7 are water)"),
            ("FOOT", "Foot sound", "Footstep sound"),
            ("GROUND", "Ground", "Ground value"),
            ("MASK", "Ignore mask", "Query ignore mask"),
        ),
        default="AREA",
    )
    paint_ground: IntProperty(name="Ground", min=0, max=255, default=0)
    paint_foot: IntProperty(name="Foot sound", min=0, max=255, default=0)
    paint_area: IntProperty(name="Area", min=0, max=255, default=6)
    paint_mask: IntProperty(name="Ignore mask", min=0, max=255, default=0)
    show_raw: BoolProperty(name="All values", description="Show every value of extras.c2", default=False)
    show_boxes: BoolProperty(name="Invent boxes", description="Draw invent points' boxes (data min, max)", default=True)
    show_anime: BoolProperty(
        name="Animations",
        description="Draw what anime points move: a position's path, a rotation's axis and arc, and a "
        "line from the selected point to what it moves",
        default=True,
    )
    hour: IntProperty(
        name="Hour",
        description="The time of day to show the map at: pieces with hours (towns' day and night "
        "versions) show only when the game draws them",
        min=0,
        max=23,
        default=12,
        update=_show_hour,
    )
    by_hour: BoolProperty(
        name="By hour",
        description="Show only the pieces the game draws at the hour and at the start (not those "
        "it hides until a script shows them, nor deleted placements); off shows every piece",
        default=True,
        update=_show_hour,
    )

    edit_selected: BoolProperty(
        name="All selected",
        description="A field's edit changes every selected object of the active one's role (and a point's kind), "
        "not only the active one",
        default=False,
    )
    outline_filter: EnumProperty(name="Show", items=[f for f in _outline_filters()], default="PLACEMENTS")
    outline_kind: EnumProperty(
        name="Kind",
        items=[("ALL", "Every kind", "")] + [(kind, schema.KIND_LABELS[kind], "") for kind in schema.POINT_KINDS],
        default="ALL",
    )
    outline_search: StringProperty(name="Find", description="Part of a name, template, kind or model", options={"TEXTEDIT_UPDATE"})
    outline_index: IntProperty(name="Listed", default=-1, update=_select_listed)
    outline_referenced: BoolProperty(
        name="Named by scripts", description="Only what the map's scripts name (read with Read Map Scripts)", default=False
    )
    outline_counts: BoolProperty(name="Instances", description="Show how many placements each template has", default=False)
    show_walkable: BoolProperty(
        name="Walkable",
        description="Tint shown floor collision: green where the player can stand, red for walls (the engine's "
        "slope limit), blue for water",
        default=False,
        update=_redraw_overlay,
    )
    show_cells: BoolProperty(
        name="Cells",
        description="Draw a dungeon part's 320 x 320 cell and its closed sides (MOVE_FLAG) around each template",
        default=True,
        update=_redraw_overlay,
    )
    show_spots: BoolProperty(
        name="Script spots",
        description="Mark where the map's scripts place the player (Read Map Scripts first)",
        default=True,
        update=_redraw_overlay,
    )
    show_cfg: BoolProperty(
        name="Water, culling",
        description="Draw what the map's .cfg adds: water surfaces, occlusion planes, areas drawn without their "
        "parts, extra trigger points and where villagers stand",
        default=True,
        update=_redraw_overlay,
    )
    show_camera_boxes: BoolProperty(
        name="Camera boxes", description="Draw the fixed cameras' FIX_CAMERA_RECT boxes", default=True, update=_redraw_overlay
    )
    show_fog: BoolProperty(
        name="Fog", description="Draw the light set's fog in Material Preview and Rendered", default=True, update=_show_fog
    )
    test_play: BoolProperty(
        name="Boot in PCSX2", description="Boot the test image in PCSX2 once it is built (build --play)", default=False
    )
    play_start: EnumProperty(
        name="Start", description="Where Play in Game puts the player",
        items=(
            ("CURSOR", "3D cursor", "Enter the map with the player at the 3D cursor, facing its way", "PIVOT_CURSOR", 0),
            ("PLAYER", "Where the player is", "Reload the map with the player where they stand in the game", "USER", 1),
        ),
        default="CURSOR",
    )
    place_name: StringProperty(name="Name", description="The game's name (31 bytes at most)", get=get_node_name, set=set_node_name)
    place_template: StringProperty(name="Template", description="PARTS_NAME: the template this placement copies", get=get_template, set=set_template)
    place_show: BoolProperty(name="Shown", description="MAP_PARTS show flag", get=get_show, set=set_show)
    place_group: StringProperty(name="Group", description="PARTS_GROUP: switched by fixed cameras", get=get_group, set=set_group)
    place_far_distance: FloatProperty(name="Far clip distance", get=get_far_distance, set=set_far_distance)
    place_far_fade: FloatProperty(name="Far clip fade", get=get_far_fade, set=set_far_fade)
    piece_name: StringProperty(name="Name", description="The game's name (31 bytes at most)", get=get_node_name, set=set_node_name)
    piece_model: StringProperty(name="Model", description="The .mds (or .chr) file", get=get_model, set=set_model)
    piece_show: BoolProperty(name="Shown", description="PIECE show flag", get=get_show, set=set_show)
    piece_time: IntVectorProperty(name="Hours", size=2, min=0, max=255, description="PIECE_TIME start, end (equal: always)", get=get_piece_hours, set=set_piece_hours)
    piece_col_type: IntProperty(name="Collision type", description="PIECE_COL_TYPE: 0 is normal; non-zero hides the piece from collision queries", get=get_collision_type, set=set_active_collision_type)
    point_name: StringProperty(name="Name", description="The game's name (31 bytes at most)", get=get_node_name, set=set_node_name)
    point_kind: EnumProperty(
        name="Kind", items=[(kind, kind, schema.KIND_LABELS[kind]) for kind in schema.POINT_KINDS],
        get=get_kind, set=set_kind,
    )
    point_enable: BoolProperty(name="Enabled", description="FUNC_DATA enable flag", get=get_enable, set=set_enable)
    point_flags: IntVectorProperty(name="Flags", size=2, description="FUNC_FLAG's first two numbers", get=get_point_flags, set=set_point_flags)
    point_hours: IntVectorProperty(name="Hours", size=2, min=0, max=255, description="FUNC_FLAG start, end hour (equal: always)", get=get_point_hours, set=set_point_hours)
    d_invent_id: IntProperty(
        name="Invent id", get=get_invent_id, set=set_invent_id,
    )
    d_invent_min: FloatVectorProperty(
        name="Box min", size=3, get=get_invent_min, set=set_invent_min,
    )
    d_invent_max: FloatVectorProperty(
        name="Box max", size=3, get=get_invent_max, set=set_invent_max,
    )
    d_invent_unknown_int: IntProperty(
        name="Int", get=get_invent_unknown_int, set=set_invent_unknown_int,
    )
    d_invent_unknown_float: FloatProperty(
        name="Float", get=get_invent_unknown_float, set=set_invent_unknown_float,
    )
    d_invent_angle: FloatProperty(
        name="Angle (degrees)", get=get_invent_angle, set=set_invent_angle,
    )
    d_event_kind: StringProperty(
        name="Event kind", get=get_event_kind, set=set_event_kind,
    )
    d_event_event_no: IntProperty(
        name="Event number", get=get_event_event_no, set=set_event_event_no,
    )
    d_event_program: IntProperty(
        name="Program", get=get_event_program, set=set_event_program,
    )
    d_event_args: IntVectorProperty(
        name="Arguments", size=3, get=get_event_args, set=set_event_args,
    )
    d_event_target: StringProperty(
        name="Target", get=get_event_target, set=set_event_target,
    )
    d_event_flags: IntVectorProperty(
        name="Flags", size=3, get=get_event_flags, set=set_event_flags,
    )
    d_flare_color: IntVectorProperty(
        name="Colour", size=3, get=get_flare_color, set=set_flare_color,
    )
    d_flare_draw: IntProperty(
        name="Draw", get=get_flare_draw, set=set_flare_draw,
    )
    d_flare_extra: IntVectorProperty(
        name="Extra", size=2, get=get_flare_extra, set=set_flare_extra,
    )
    d_fire_color: IntVectorProperty(
        name="Colour", size=3, get=get_fire_color, set=set_fire_color,
    )
    d_fire_draw: IntProperty(
        name="Draw", get=get_fire_draw, set=set_fire_draw,
    )
    d_fire_extra: IntVectorProperty(
        name="Extra", size=2, get=get_fire_extra, set=set_fire_extra,
    )
    d_plight_power: FloatProperty(
        name="Power", get=get_plight_power, set=set_plight_power,
    )
    d_plight_color: IntVectorProperty(
        name="Colour (0-511)", size=3, get=get_plight_color, set=set_plight_color,
    )
    d_plight_mode: IntProperty(
        name="Mode", get=get_plight_mode, set=set_plight_mode,
    )
    d_plight_args: IntVectorProperty(
        name="Arguments", size=4, get=get_plight_args, set=set_plight_args,
    )
    d_plight_flicker: IntProperty(
        name="Flicker type", get=get_plight_flicker, set=set_plight_flicker,
    )
    d_plight_depth: FloatProperty(
        name="Flicker depth", get=get_plight_depth, set=set_plight_depth,
    )
    d_plight_period: FloatProperty(
        name="Flicker period", get=get_plight_period, set=set_plight_period,
    )
    d_effect_effect: StringProperty(
        name="Effect name", get=get_effect_effect, set=set_effect_effect,
    )
    d_sound_number: IntProperty(
        name="Sound number", get=get_sound_number, set=set_sound_number,
    )
    d_sound_near: FloatProperty(
        name="Near", get=get_sound_near, set=set_sound_near,
    )
    d_sound_far: FloatProperty(
        name="Far", get=get_sound_far, set=set_sound_far,
    )
    d_sound_unknown_int: IntProperty(
        name="Int", get=get_sound_unknown_int, set=set_sound_unknown_int,
    )
    d_sound_shape: IntProperty(
        name="Shape", get=get_sound_shape, set=set_sound_shape,
    )
    d_sound_start: FloatVectorProperty(
        name="Start", size=3, get=get_sound_start, set=set_sound_start,
    )
    d_sound_end: FloatVectorProperty(
        name="End", size=3, get=get_sound_end, set=set_sound_end,
    )
    d_anime_placement: StringProperty(
        name="Placement", get=get_anime_placement, set=set_anime_placement,
    )
    d_anime_piece: StringProperty(
        name="Piece", get=get_anime_piece, set=set_anime_piece,
    )
    d_anime_frame: StringProperty(
        name="Model node", get=get_anime_frame, set=set_anime_frame,
    )
    d_anime_drives: EnumProperty(
        name="Drives",
        items=[(identifier, label, description, value) for value, identifier, label, description in schema.ANIME_DRIVES],
        get=get_anime_drives, set=set_anime_drives,
    )
    d_anime_mode: EnumProperty(
        name="Mode",
        items=[(identifier, label, description, value) for value, identifier, label, description in schema.ANIME_MODES],
        get=get_anime_mode, set=set_anime_mode,
    )
    d_anime_start: FloatVectorProperty(
        name="Start", size=3, get=get_anime_start, set=set_anime_start,
    )
    d_anime_step: FloatVectorProperty(
        name="Step (per frame)", size=3, get=get_anime_step, set=set_anime_step,
    )
    d_anime_end: FloatVectorProperty(
        name="End", size=3, get=get_anime_end, set=set_anime_end,
    )
    d_anime_uniform: BoolProperty(
        name="Uniform (Y and Z follow X)", get=get_anime_uniform, set=set_anime_uniform,
    )
    d_anime_relative: BoolProperty(
        name="Relative (in the piece's own space)", get=get_anime_relative, set=set_anime_relative,
    )


def _more_settings():
    """The settings other modules define (lighting) and the template library's."""
    from . import lighting

    found = dict(lighting.lighting_properties())
    found.update(lighting.light_props())
    found["library_map"] = StringProperty(
        name="From map", description="The map to take a template from: pick it from the list, or type its name",
    )
    found["library_templates"] = CollectionProperty(type=Dc2tkMapEntry)
    found["library_template"] = StringProperty(name="Template", description="The template to place")
    return found


Dc2tkSettings.__annotations__.update(_more_settings())

classes = (Dc2tkMapEntry, Dc2tkSettings)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.WindowManager.dc2tk_maps = PointerProperty(type=Dc2tkSettings)


def unregister():
    del bpy.types.WindowManager.dc2tk_maps
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)


def settings(context=None):
    return (context or bpy.context).window_manager.dc2tk_maps
