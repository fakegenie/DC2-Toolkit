import json

import bpy

from . import budgets, c2data, edit, import_map, outliner, prefs, props, runner, schema, scripts, validate

CATEGORY = "Dark Chronicle"
ROLE_LABELS = {
    "map": "Map root",
    "placement": "Placement",
    "piece": "Piece",
    "mds": "Model node",
    "point": "Trigger point",
    "group": "Group",
    "camera": "Fixed camera",
}
ROLE_ICONS = {
    "map": "WORLD",
    "placement": "OUTLINER_OB_EMPTY",
    "piece": "OBJECT_DATA",
    "mds": "MESH_DATA",
    "point": "EMPTY_AXIS",
    "group": "GROUP",
    "camera": "CAMERA_DATA",
}


def _is_scalar(value):
    return value is None or isinstance(value, (bool, int, float, str))


def _is_flat(value):
    return isinstance(value, list) and all(_is_scalar(item) for item in value)


def draw_value_row(layout, obj, path, label, value):
    row = layout.row(align=True)
    split = row.split(factor=0.45, align=True)
    split.label(text=label)
    split.label(text=edit.show_value(value))
    op = row.operator(edit.DC2TK_OT_edit_value.bl_idname, text="", icon="GREASEPENCIL")
    op.object_name = obj.name
    op.path = json.dumps(path)


def draw_tree(layout, obj, path, value, label=None, depth=0):
    if _is_scalar(value) or _is_flat(value):
        draw_value_row(layout, obj, path, label or str(path[-1]), value)
        return
    key = obj.name + json.dumps(path)
    open_ = key in edit.expanded or depth == 0
    if depth > 0:
        row = layout.row(align=True)
        op = row.operator(
            edit.DC2TK_OT_toggle_expand.bl_idname,
            text="%s (%d)" % (label or path[-1], len(value)),
            icon="TRIA_DOWN" if open_ else "TRIA_RIGHT",
            emboss=False,
        )
        op.key = key
    if not open_:
        return
    box = layout.box() if depth > 0 else layout
    col = box.column(align=True)
    items = value.items() if isinstance(value, dict) else enumerate(value)
    for child_key, child in items:
        child_label = schema.map_label(child_key) if isinstance(child_key, str) else "[%d]" % child_key
        draw_tree(col, obj, path + [child_key], child, child_label, depth + 1)


def draw_vector(layout, settings, prop_name, field):
    col = layout.column(align=True)
    col.label(text=field["label"])
    row = col.row(align=True) if field["size"] <= 3 and not field["labels"] else col.column(align=True)
    for index in range(field["size"]):
        text = field["labels"][index] if field["labels"] else ""
        row.prop(settings, prop_name, index=index, text=text)


def draw_point(layout, obj, settings):
    data = c2data.get(obj) or {}
    kind = data.get("kind")
    col = layout.column(align=True)
    col.prop(settings, "point_name")
    col.prop(settings, "point_kind")
    col.prop(settings, "point_enable")
    col = layout.column(align=True)
    col.prop(settings, "point_hours")
    col.prop(settings, "point_flags")
    if kind not in schema.POINT_KINDS:
        layout.label(text="Unknown kind %r" % (kind,), icon="ERROR")
        return
    box = layout.box()
    box.label(text=schema.KIND_LABELS[kind], icon="PROPERTIES")
    if kind == "anime":
        draw_anime(box, obj, settings)
        return
    for field in schema.DATA_FIELDS[kind]:
        prop_name = props.data_prop_name(kind, field)
        if kind == "event" and field["key"] == "kind":
            row = box.row(align=True)
            row.prop(settings, prop_name)
            row.menu(edit.DC2TK_MT_event_kinds.bl_idname, text="", icon="DOWNARROW_HLT")
            continue
        if kind == "event" and field["key"] == "program":
            row = box.row(align=True)
            row.prop(settings, prop_name)
            row.menu("DC2TK_MT_programs", text="", icon="DOWNARROW_HLT")
            given = point_program_name(obj)
            if given:
                box.label(text="mod.json gives it %s (build numbers it)" % given, icon="TEXT")
            continue
        if field["size"] > 1:
            draw_vector(box, settings, prop_name, field)
        else:
            box.prop(settings, prop_name)
    if kind == "event":
        size = schema.EVENT_DEFAULT_SIZE.get(c2data.data_field(obj, schema.DATA_FIELDS["event"][0]))
        if size:
            box.label(text="Size %g when the scale is 1" % size, icon="INFO")
    if kind == "plight" and obj.type == "LIGHT":
        light = box.column(align=True)
        light.label(text="Blender light (KHR_lights_punctual)", icon="LIGHT_POINT")
        light.prop(obj.data, "color")
        light.prop(obj.data, "energy")
        light.prop(obj.data, "use_custom_distance", text="Range")
        if obj.data.use_custom_distance:
            light.prop(obj.data, "cutoff_distance")
    elif kind == "plight":
        box.label(text="Not a Blender light: re-import or add it again", icon="ERROR")


def point_program_name(obj):
    """The program the mod's mod.json gives an event point by name, if any."""
    from . import export_mod

    mod_dir = prefs.absolute(prefs.get().mod_dir) if isinstance(prefs.get(), bpy.types.AddonPreferences) else ""
    if not mod_dir:
        return None
    root = c2data.map_root(obj)
    return export_mod.programs_of(mod_dir.rstrip("\\/"), c2data.map_name(root)).get(c2data.exact_name(obj, "point"))


def draw_anime(box, obj, settings):
    fields = {field["key"]: field for field in schema.DATA_FIELDS["anime"]}

    def name(key):
        return props.data_prop_name("anime", fields[key])

    col = box.column(align=True)
    if c2data.placement_of(obj) is not None:
        col.label(text="Moves its own placement", icon="OUTLINER_OB_EMPTY")
    else:
        col.prop(settings, name("placement"))
    col.prop(settings, name("piece"))
    col.prop(settings, name("frame"))
    target, problem = c2data.anime_target(obj)
    if target is not None:
        col.label(text="Moves %s" % target.name, icon="CHECKMARK")
    else:
        col.label(text="Moves nothing: %s" % problem, icon="ERROR")
    col = box.column(align=True)
    col.prop(settings, name("drives"))
    col.prop(settings, name("mode"))
    drives = getattr(settings, name("drives"))
    mode = getattr(settings, name("mode"))
    drives_value = next(choice[0] for choice in schema.ANIME_DRIVES if choice[1] == drives)
    mode_choice = next(choice for choice in schema.ANIME_MODES if choice[1] == mode)
    box.label(text=mode_choice[3], icon="INFO")
    for key, (label, elements) in schema.anime_vectors(drives_value, mode_choice[0]).items():
        col = box.column(align=True)
        col.label(text=label)
        row = col.row(align=True)
        for index, element in enumerate(elements):
            row.prop(settings, name(key), index=index, text=element)
    col = box.column(align=True)
    col.prop(settings, name("uniform"))
    if drives_value == 1:
        col.prop(settings, name("relative"))


def draw_placement(layout, obj, settings):
    col = layout.column(align=True)
    col.prop(settings, "place_name")
    col.prop(settings, "place_template")
    col.prop(settings, "place_group")
    col.prop(settings, "place_show")
    far = c2data.to_py(c2data.lookup(obj, ("far_clip",)))
    box = layout.box()
    if far is None:
        row = box.row()
        row.label(text="Far clip: none")
        op = row.operator(edit.DC2TK_OT_set_value.bl_idname, text="Add", icon="ADD")
        op.object_name, op.path, op.json_value = obj.name, json.dumps(["far_clip"]), "[1000.0, 100.0]"
    else:
        col = box.column(align=True)
        col.prop(settings, "place_far_distance")
        col.prop(settings, "place_far_fade")
        op = box.operator(edit.DC2TK_OT_clear_value.bl_idname, text="No far clip", icon="X")
        op.object_name, op.path = obj.name, json.dumps(["far_clip"])
    if c2data.lookup(obj, ("preview",)):
        layout.label(text="Template %s, shown once: edits change it" % c2data.lookup(obj, ("template",)))
        layout.label(text="It places nothing; moving it changes nothing")
        box = layout.box()
        box.label(text="Closed sides of its dungeon cell (MOVE_FLAG)", icon="MOD_BUILD")
        row = box.row(align=True)
        flags = c2data.to_py(c2data.lookup(obj, ("move_flag",)))
        flags = list(flags) if isinstance(flags, list) else [0, 0, 0, 0]
        for side, label in enumerate(("-X", "-Z", "+X", "+Z")):
            op = row.operator(
                edit.DC2TK_OT_set_value.bl_idname, text=label, depress=bool(flags[side] if side < len(flags) else 0)
            )
            changed = list(flags) + [0] * (4 - len(flags))
            changed[side] = 0 if changed[side] else 1
            op.object_name, op.path, op.json_value = obj.name, json.dumps(["move_flag"]), json.dumps(changed)
        return
    slot = c2data.lookup(obj, ("slot",))
    layout.label(text="Slot %s" % slot if slot is not None else "New: appended after the map's placements")
    layout.operator(edit.DC2TK_OT_duplicate_placement.bl_idname, icon="DUPLICATE")


_instances = {"key": None, "groups": []}


def draw_instances(layout, obj):
    """How the build will treat the template of obj's placement: changed for all its placements,
    or split into copies when they differ."""
    placement = c2data.placement_of(obj)
    if placement is None or c2data.is_preview(placement):
        return
    # comparing every placement of the template is worth doing once a change, not every redraw
    key = (placement.name, outliner.generation["count"])
    if _instances.get("key") != key:
        _instances["key"], _instances["groups"] = key, [[o.name for o in group] for group in c2data.instance_groups(placement)]
    groups = [[bpy.data.objects[name] for name in group if name in bpy.data.objects] for group in _instances["groups"]]
    count = sum(len(group) for group in groups)
    box = layout.box()
    template = c2data.template_name(placement)
    if count == 1:
        box.label(text="Template %s: this placement only" % template, icon="OUTLINER_OB_EMPTY")
        return
    if len(groups) == 1:
        box.label(text="Template %s: %d placements, alike" % (template, count), icon="LINKED")
        box.label(text="Edits here change the template only if made to all")
    else:
        box.label(text="Template %s: %d placements differ" % (template, count), icon="ERROR")
        box.label(text="The build gives %d of them templates of their own" % (count - len(groups[0])))
        box.label(text="This one is alike with %d" % (len(groups[0]) - 1))
    row = box.row(align=True)
    row.operator("dc2tk.apply_to_instances", text="Apply to All", icon="LINKED")
    row.operator("dc2tk.make_unique", text="Make Unique", icon="UNLINKED")
    row.operator("dc2tk.select_instances", text="", icon="RESTRICT_SELECT_OFF")


def draw_references(layout, obj):
    found = scripts.references(obj)
    if not found:
        return
    box = layout.box()
    box.label(text="Named by %d script calls: keep its name" % len(found), icon="TEXT")
    col = box.column(align=True)
    for use in found[:6]:
        col.label(text="%s:%s  %s" % (use.get("path", "?").rsplit("/", 1)[-1], use.get("line", "?"), use.get("call", "")))
    if len(found) > 6:
        col.label(text="... %d more" % (len(found) - 6))


def draw_piece(layout, obj, settings):
    data = c2data.get(obj) or {}
    col = layout.column(align=True)
    col.prop(settings, "piece_name")
    col.prop(settings, "piece_model")
    col.label(text="Kind: %s" % data.get("kind", "?"))
    col.prop(settings, "piece_show")
    col.prop(settings, "piece_time")
    col.prop(settings, "piece_col_type")
    materials = data.get("materials")
    if materials:
        layout.label(text="%d material overrides (All values)" % len(materials))


class DC2TK_PT_main(bpy.types.Panel):
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = CATEGORY

    bl_label = "Map"
    bl_idname = "DC2TK_PT_main"

    def draw(self, context):
        layout = self.layout
        settings = props.settings(context)
        found = prefs.dc2tk_command(prefs.get(context)) is not None
        if found and not settings.maps:
            import_map.load_map_list_once()
        col = layout.column(align=True)
        row = col.row(align=True)
        row.prop_search(settings, "import_name", settings, "maps", text="", icon="WORLD")
        row.operator(import_map.DC2TK_OT_load_maps.bl_idname, text="", icon="FILE_REFRESH")
        op = col.operator(import_map.DC2TK_OT_import_map.bl_idname, text="Import", icon="IMPORT")
        op.map_name = settings.import_name
        col.operator(import_map.DC2TK_OT_import_gltf.bl_idname, text="Import exported .gltf", icon="FILE_FOLDER")
        if not found:
            col.label(text="Set the dc2tk repository in the preferences", icon="ERROR")
        elif c2data.scene_map_roots(context.scene):
            col.label(text="Importing replaces the map in the scene", icon="INFO")

        root = c2data.current_map_root(context)
        if root is None:
            layout.label(text="No map in the scene")
            return
        box = layout.box()
        box.label(text=root.name, icon="WORLD")
        box.prop(settings, "select_level", expand=True)
        row = box.row(align=True)
        for kind, label in (("collision", "Collision"), ("camera", "Camera col.")):
            visible = import_map.collision_visible(context, root, kind)
            op = row.operator(
                edit.DC2TK_OT_toggle_collision.bl_idname,
                text=label,
                icon="HIDE_OFF" if visible else "HIDE_ON",
                depress=bool(visible),
            )
            op.kind = kind
        row = box.row(align=True)
        row.prop(settings, "collision_colour", text="")
        row.operator(edit.DC2TK_OT_recolour_collision.bl_idname, text="", icon="COLOR")
        row = box.row(align=True)
        row.prop(settings, "show_walkable", icon="MOD_PHYSICS")
        row.prop(settings, "show_fog", icon="MOD_FLUIDSIM")
        row = box.row(align=True)
        row.prop(settings, "show_boxes")
        row.prop(settings, "show_anime")
        row = box.row(align=True)
        row.prop(settings, "show_camera_boxes")
        row.prop(settings, "show_spots")
        if c2data.lookup(root, ("cfg",)) is not None:
            box.prop(settings, "show_cfg")
        found = budgets.figures(root)
        if found["templates"]:
            box.prop(settings, "show_cells", text="Dungeon cells and closed sides")
        row = box.row(align=True)
        row.prop(settings, "by_hour", text="", icon="TIME")
        sub = row.row(align=True)
        sub.enabled = settings.by_hour
        sub.prop(settings, "hour")
        row = box.row(align=True)
        row.operator("dc2tk.play_animations", icon="PAUSE" if animation_playing() else "PLAY")
        row.operator("dc2tk.read_scripts", icon="TEXT")
        counts = box.column(align=True)
        counts.label(text="%d placements, %d points" % (found["placements"], found["points"]))


def animation_playing():
    from . import animate

    return animate.playing()


class DC2TK_PT_outliner(bpy.types.Panel):
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = CATEGORY
    bl_label = "Map Outliner"
    bl_idname = "DC2TK_PT_outliner"
    bl_options = {"DEFAULT_CLOSED"}

    @classmethod
    def poll(cls, context):
        return c2data.current_map_root(context) is not None

    def draw(self, context):
        outliner.draw(self.layout, context)


class DC2TK_PT_tools(bpy.types.Panel):
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = CATEGORY
    bl_label = "Lay Out"
    bl_idname = "DC2TK_PT_tools"

    @classmethod
    def poll(cls, context):
        return c2data.current_map_root(context) is not None

    def draw(self, context):
        layout = self.layout
        col = layout.column(align=True)
        col.operator("dc2tk.drop_to_ground", icon="TRIA_DOWN_BAR")
        row = col.row(align=True)
        row.operator("dc2tk.game_snapping", text="Town snap").preset = "TOWN"
        row.operator("dc2tk.game_snapping", text="Cell snap").preset = "CELL"
        row.operator("dc2tk.game_snapping", text="", icon="SNAP_OFF").preset = "FREE"
        col = layout.column(align=True)
        col.operator_menu_enum("dc2tk.copy_coordinates", "form", text="Copy Game Coordinates", icon="COPYDOWN")
        col.operator("dc2tk.player_reference", icon="ARMATURE_DATA")
        settings = context.window_manager.dc2tk_maps
        box = layout.box()
        box.label(text="Templates of any map", icon="ASSET_MANAGER")
        row = box.row(align=True)
        row.prop_search(settings, "library_map", settings, "maps", text="", icon="WORLD")
        row.operator("dc2tk.load_library", text="", icon="FILE_REFRESH")
        box.prop_search(settings, "library_template", settings, "library_templates", text="", icon="OUTLINER_OB_EMPTY")
        box.operator("dc2tk.add_template", text="Place at Cursor", icon="ADD")
        col = layout.column(align=True)
        col.operator("dc2tk.add_door_to_map", icon="EXPORT")
        col.operator("dc2tk.new_map", icon="FILE_NEW")
        col = layout.column(align=True)
        col.operator("dc2tk.collision_from_mesh", icon="MOD_PHYSICS")
        col.operator("dc2tk.split_collision", icon="MOD_EXPLODE")
        col.operator("dc2tk.layout_preview", icon="VIEW_ORTHO")


class DC2TK_PT_lighting(bpy.types.Panel):
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = CATEGORY
    bl_label = "Lighting"
    bl_idname = "DC2TK_PT_lighting"
    bl_options = {"DEFAULT_CLOSED"}

    @classmethod
    def poll(cls, context):
        return c2data.current_map_root(context) is not None

    def draw(self, context):
        from . import lighting

        if c2data.current_map_root(context) is None:
            self.layout.label(text="No map in the scene")
            return
        lighting.draw(self.layout, context)


class DC2TK_PT_budgets(bpy.types.Panel):
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = CATEGORY
    bl_label = "Budgets"
    bl_idname = "DC2TK_PT_budgets"
    bl_options = {"DEFAULT_CLOSED"}

    @classmethod
    def poll(cls, context):
        return c2data.current_map_root(context) is not None

    def draw(self, context):
        budgets.draw(self.layout, context, c2data.current_map_root(context))


class DC2TK_PT_selected(bpy.types.Panel):
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = CATEGORY

    bl_label = "Selected"
    bl_idname = "DC2TK_PT_selected"

    def draw(self, context):
        layout = self.layout
        obj = context.active_object
        settings = props.settings(context)
        if obj is None:
            layout.label(text="Nothing active")
            return
        if c2data.is_stringified(obj):
            layout.label(text="extras.c2 is kept as text", icon="ERROR")
            layout.label(text="(Blender could not store it); not editable")
            return
        data = c2data.get(obj)
        if data is None:
            layout.label(text="%s: no extras.c2" % obj.name, icon="QUESTION")
            layout.operator_menu_enum(edit.DC2TK_OT_add_c2.bl_idname, "role", text="Add c2 extras")
            return
        role = data.get("role")
        layout.label(text="%s: %s" % (ROLE_LABELS.get(role, role), obj.name), icon=ROLE_ICONS.get(role, "QUESTION"))
        alike = [o for o in context.selected_objects if o is not obj and c2data.role(o) == role]
        if alike and role in ("placement", "piece", "point"):
            layout.prop(settings, "edit_selected", text="Edit all %d selected" % (len(alike) + 1), icon="RESTRICT_SELECT_OFF")
        if role in ("placement", "piece", "point"):
            draw_references(layout, obj)
        if role == "placement":
            draw_placement(layout, obj, settings)
            draw_instances(layout, obj)
        elif role == "piece":
            draw_piece(layout, obj, settings)
            draw_instances(layout, obj)
        elif role == "point":
            draw_point(layout, obj, settings)
            draw_instances(layout, obj)
        elif role == "mds":
            layout.label(text="mds_index %s" % data.get("mds_index", "(new)"))
            if c2data.is_collision_mesh(obj):
                layout.label(text="Collision mesh: see Add > Collision", icon="MOD_PHYSICS")
                layout.operator("dc2tk.split_collision", icon="MOD_EXPLODE")
            elif obj.type == "MESH":
                from . import views

                views.draw_flags(layout, obj)
                layout.operator("dc2tk.collision_from_mesh", icon="MOD_PHYSICS")
        elif role == "map":
            layout.label(text="Settings: the Map Settings and Lighting panels")
        elif role == "camera":
            layout.operator("dc2tk.view_camera", icon="CAMERA_DATA")
            draw_tree(layout, obj, [], data)
        layout.prop(settings, "show_raw")
        if settings.show_raw and role != "camera":
            draw_tree(layout.box(), obj, [], {k: v for k, v in data.items() if k != "map"})


class DC2TK_PT_add(bpy.types.Panel):
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = CATEGORY

    bl_label = "Add"
    bl_idname = "DC2TK_PT_add"

    def draw(self, context):
        layout = self.layout
        settings = props.settings(context)
        col = layout.column(align=True)
        col.label(text="Point at the 3D cursor:")
        grid = col.grid_flow(columns=2, align=True)
        for preset in schema.POINT_PRESETS:
            grid.operator(edit.DC2TK_OT_add_point.bl_idname, text=preset[1]).preset = preset[0]
        placement = c2data.placement_of(context.active_object)
        col.label(text="Into %s" % (placement.name if placement else "the map's points"), icon="INFO")
        layout.operator(edit.DC2TK_OT_duplicate_placement.bl_idname, icon="DUPLICATE")
        layout.operator(edit.DC2TK_OT_new_model.bl_idname, icon="MESH_CUBE")
        if not edit.loose_meshes(context):
            layout.label(text="Model a mesh, select it, then make it a model", icon="INFO")

        box = layout.box()
        box.label(text="Collision", icon="MOD_PHYSICS")
        col = box.column(align=True)
        col.prop(settings, "paint_ground")
        col.prop(settings, "paint_foot")
        col.prop(settings, "paint_area")
        col.prop(settings, "paint_mask")
        name = schema.col_name(settings.paint_ground, settings.paint_foot, settings.paint_area, settings.paint_mask)
        box.label(text=name)
        op = box.operator(edit.DC2TK_OT_paint_collision.bl_idname, text="Set selected faces", icon="BRUSH_DATA")
        op.ground, op.foot = settings.paint_ground, settings.paint_foot
        op.area, op.mask = settings.paint_area, settings.paint_mask
        obj = context.active_object
        if obj is not None and obj.type == "MESH" and c2data.is_collision_mesh(obj) and obj.active_material:
            attrs = schema.parse_col(obj.active_material.name)
            if attrs:
                box.label(text="Active: ground %d, foot %d, area %d, mask %d" % attrs)


class DC2TK_PT_map_settings(bpy.types.Panel):
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = CATEGORY

    bl_label = "Map Settings"
    bl_idname = "DC2TK_PT_map_settings"
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        root = c2data.current_map_root(context)
        if root is None:
            layout.label(text="No map in the scene")
            return
        if c2data.is_stringified(root):
            layout.label(text="extras.c2 is kept as text; not editable", icon="ERROR")
            return
        tree = c2data.to_py(c2data.lookup(root, ("map",)))
        if not isinstance(tree, dict):
            layout.label(text="No map settings (extras.c2.map)", icon="INFO")
            return
        layout.label(text="Light sets, fog, background, time, sky...", icon="LIGHT_SUN")
        draw_tree(layout, root, ["map"], tree)


class DC2TK_PT_export(bpy.types.Panel):
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = CATEGORY

    bl_label = "Check, Export, Build"
    bl_idname = "DC2TK_PT_export"

    def draw(self, context):
        layout = self.layout
        settings = prefs.get(context)
        layout.operator(validate.DC2TK_OT_validate.bl_idname, icon="CHECKMARK")
        if validate.results:
            box = layout.box()
            col = box.column(align=True)
            for level, name, message in validate.results[:12]:
                row = col.row(align=True)
                row.label(text=message, icon="ERROR" if level == validate.ERROR else "INFO")
                op = row.operator(validate.DC2TK_OT_select_problem.bl_idname, text="", icon="RESTRICT_SELECT_OFF")
                op.object_name = name
            if len(validate.results) > 12:
                col.label(text="... %d more (text \"dc2tk validation\")" % (len(validate.results) - 12))
        has_prefs = isinstance(settings, bpy.types.AddonPreferences)
        box = layout.box()
        box.label(text="Mod", icon="EXPORT")
        if has_prefs:
            box.prop(settings, "mod_dir", text="")
        box.operator("dc2tk.export_mod", icon="EXPORT")
        box = layout.box()
        box.label(text="Try it", icon="PLAY")
        from . import play

        play.draw(box, context)
        row = box.row(align=True)
        row.operator("dc2tk.preview_changes", icon="VIEWZOOM")
        row.operator("dc2tk.test_here", icon="PLAY")
        box.prop(context.window_manager.dc2tk_maps, "test_play")
        from . import testing

        testing.draw_preview(box)
        box = layout.box()
        box.label(text="Build", icon="DISC")
        if has_prefs:
            col = box.column(align=True)
            col.prop(settings, "iso_path", text="Retail disc")
            col.prop(settings, "output_iso", text="Output ISO")
            col.prop(settings, "source_dir", text="Source")
        box.operator("dc2tk.build", icon="PLAY")
        lines = runner.tail()
        if lines or runner.current is not None:
            out = box.column(align=True)
            job = runner.current
            state = "running" if job["returncode"] is None else "exit %d" % job["returncode"]
            out.label(text="%s: %s" % (job["title"], state), icon="CONSOLE")
            for line in lines:
                out.label(text=line)
            box.operator("dc2tk.show_output", icon="TEXT")


classes = (
    DC2TK_PT_main, DC2TK_PT_outliner, DC2TK_PT_selected, DC2TK_PT_tools, DC2TK_PT_add, DC2TK_PT_lighting,
    DC2TK_PT_map_settings, DC2TK_PT_budgets, DC2TK_PT_export,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
