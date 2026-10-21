"""The add-on in Blender's own menus: Add (Shift+A) > Dark Chronicle, the object context menu
(right click), and a pie menu of the common actions (Shift+Alt+D)."""

import bpy

from . import c2data, schema

PIE_KEY = ("D", True, True)  # key, shift, alt
_keymaps = []


class DC2TK_MT_add(bpy.types.Menu):
    bl_idname = "DC2TK_MT_add"
    bl_label = "Dark Chronicle"

    def draw(self, context):
        layout = self.layout
        layout.operator_context = "EXEC_DEFAULT"
        for preset in schema.POINT_PRESETS:
            icon = {"plight": "LIGHT_POINT", "sound": "SPEAKER", "anime": "ANIM", "event": "EVENT_E"}.get(preset[2], "EMPTY_AXIS")
            layout.operator("dc2tk.add_point", text=preset[1], icon=icon).preset = preset[0]
        layout.separator()
        layout.operator_context = "INVOKE_DEFAULT"
        layout.operator("dc2tk.add_door_to_map", icon="EXPORT")
        layout.operator("dc2tk.add_template", icon="ASSET_MANAGER")
        layout.operator("dc2tk.duplicate_placement", icon="DUPLICATE")
        layout.operator("dc2tk.new_model", icon="MESH_CUBE")
        layout.separator()
        layout.operator("dc2tk.player_reference", icon="ARMATURE_DATA")


class DC2TK_MT_context(bpy.types.Menu):
    bl_idname = "DC2TK_MT_context"
    bl_label = "Dark Chronicle"

    def draw(self, context):
        layout = self.layout
        layout.operator("dc2tk.drop_to_ground", icon="TRIA_DOWN_BAR")
        layout.operator_menu_enum("dc2tk.copy_coordinates", "form", text="Copy Game Coordinates", icon="COPYDOWN")
        layout.separator()
        layout.operator("dc2tk.select_instances", icon="RESTRICT_SELECT_OFF")
        layout.operator("dc2tk.apply_to_instances", icon="LINKED")
        layout.operator("dc2tk.make_unique", icon="UNLINKED")
        layout.operator("dc2tk.duplicate_placement", icon="DUPLICATE")


class DC2TK_MT_pie(bpy.types.Menu):
    bl_idname = "DC2TK_MT_pie"
    bl_label = "Dark Chronicle"

    def draw(self, context):
        pie = self.layout.menu_pie()
        pie.operator_context = "EXEC_DEFAULT"
        # in the pie's order: left, right, bottom, top, top left, top right, bottom left, bottom right
        pie.operator("dc2tk.add_point", text="Door", icon="EVENT_E").preset = "DOOR"
        pie.operator("dc2tk.add_point", text="Point light", icon="LIGHT_POINT").preset = "LIGHT"
        pie.operator("dc2tk.drop_to_ground", icon="TRIA_DOWN_BAR")
        pie.operator("dc2tk.add_point", text="Position (spawn)", icon="EMPTY_SINGLE_ARROW").preset = "SPAWN"
        pie.operator("dc2tk.add_point", text="Treasure chest", icon="PACKAGE").preset = "CHEST"
        pie.operator("dc2tk.duplicate_placement", icon="DUPLICATE")
        pie.operator_context = "INVOKE_DEFAULT"
        pie.operator("dc2tk.validate", icon="CHECKMARK")
        pie.operator("dc2tk.play_in_game", icon="PLAY")


def menu_add(self, context):
    self.layout.menu(DC2TK_MT_add.bl_idname, icon="WORLD")


def menu_context(self, context):
    if c2data.current_map_root(context) is not None:
        self.layout.separator()
        self.layout.menu(DC2TK_MT_context.bl_idname, icon="WORLD")


classes = (DC2TK_MT_add, DC2TK_MT_context, DC2TK_MT_pie)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.VIEW3D_MT_add.append(menu_add)
    bpy.types.VIEW3D_MT_object_context_menu.append(menu_context)
    config = bpy.context.window_manager.keyconfigs.addon
    if config is not None:
        keymap = config.keymaps.new(name="3D View", space_type="VIEW_3D")
        key, shift, alt = PIE_KEY
        item = keymap.keymap_items.new("wm.call_menu_pie", key, "PRESS", shift=shift, alt=alt)
        item.properties.name = DC2TK_MT_pie.bl_idname
        _keymaps.append((keymap, item))
        _keymaps.append((keymap, keymap.keymap_items.new("dc2tk.play_in_game", "F5", "PRESS")))


def unregister():
    for keymap, item in _keymaps:
        keymap.keymap_items.remove(item)
    _keymaps.clear()
    bpy.types.VIEW3D_MT_object_context_menu.remove(menu_context)
    bpy.types.VIEW3D_MT_add.remove(menu_add)
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
