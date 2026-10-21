import bpy

from . import c2data

PREFIX = "c2 look"
SHADER = PREFIX + " shader"
FLAG = "c2_look"


def _output(tree):
    outputs = [node for node in tree.nodes if node.type == "OUTPUT_MATERIAL"]
    for node in outputs:
        if node.is_active_output:
            return node
    return outputs[0] if outputs else None


def _first(tree, kind):
    return next((node for node in tree.nodes if node.type == kind and not node.name.startswith(PREFIX)), None)


def _node(tree, kind, label, x, y):
    node = tree.nodes.new(kind)
    node.name = "%s %s" % (PREFIX, label)
    node.label = label
    node.location = (x, y)
    return node


def _math(tree, operation, a, b, label, x, y):
    node = _node(tree, "ShaderNodeMath", label, x, y)
    node.operation = operation
    for socket, value in zip(node.inputs, (a, b)):
        if isinstance(value, bpy.types.NodeSocket):
            tree.links.new(value, socket)
        else:
            socket.default_value = value
    return node.outputs[0]


def _multiply_colour(tree, a, b, label, x, y):
    node = _node(tree, "ShaderNodeMix", label, x, y)
    node.data_type = "RGBA"
    node.blend_type = "MULTIPLY"
    node.inputs["Factor"].default_value = 1.0
    sockets = [s for s in node.inputs if s.type == "RGBA"]
    tree.links.new(a, sockets[0])
    tree.links.new(b, sockets[1])
    return next(s for s in node.outputs if s.type == "RGBA")


def is_map_material(material):
    return (
        material is not None
        and material.node_tree is not None
        and not material.name.startswith("col_")
        and ("frame_flags" in material or "texture" in material or "mdt_diffuse" in material)
    )


def apply(material):
    tree = material.node_tree
    for node in [node for node in tree.nodes if node.name.startswith(PREFIX)]:
        tree.nodes.remove(node)
    output = _output(tree)
    principled = _first(tree, "BSDF_PRINCIPLED")
    if output is None or principled is None:
        return False
    flags = c2data.to_py(material.get("frame_flags")) or {}
    if not hasattr(flags, "get"):
        flags = {}
    diffuse = c2data.to_py(material.get("mdt_diffuse")) or [1.0, 1.0, 1.0, 1.0]
    picture = _first(tree, "TEX_IMAGE")
    vertex = _first(tree, "VERTEX_COLOR")
    x, y = output.location.x - 900, output.location.y - 420

    if picture is not None:
        colour, alpha = picture.outputs["Color"], picture.outputs["Alpha"]
    else:
        base = principled.inputs["Base Color"].default_value
        rgb = _node(tree, "ShaderNodeRGB", "colour", x, y)
        rgb.outputs[0].default_value = (base[0], base[1], base[2], 1.0)
        colour, alpha = rgb.outputs[0], 1.0
    if vertex is not None:
        colour = _multiply_colour(tree, colour, vertex.outputs["Color"], "vertex colour", x + 200, y)
        alpha = _math(tree, "MULTIPLY", alpha, vertex.outputs["Alpha"], "vertex alpha", x + 200, y - 200)
    if len(diffuse) > 3 and float(diffuse[3]) < 1.0:
        alpha = _math(tree, "MULTIPLY", alpha, float(diffuse[3]), "diffuse alpha", x + 400, y - 200)
    blend = flags.get("blend")
    alpha_ref = flags.get("alpha_ref")
    if alpha_ref and blend not in ("add", "subtract") and isinstance(alpha, bpy.types.NodeSocket):
        kept = _math(tree, "GREATER_THAN", alpha, max(0.0, float(alpha_ref) - 0.5) / 128.0, "alpha test", x + 400, y - 400)
        alpha = _math(tree, "MULTIPLY", alpha, kept, "tested alpha", x + 550, y - 400)
    unlit = flags.get("lit") is False

    sx = x + 650
    if blend == "add":
        glow = _node(tree, "ShaderNodeEmission", "glow", sx, y)
        if isinstance(alpha, bpy.types.NodeSocket):
            colour = _multiply_colour(tree, colour, alpha, "glow by alpha", sx - 200, y + 200)
        tree.links.new(colour, glow.inputs["Color"])
        clear = _node(tree, "ShaderNodeBsdfTransparent", "behind", sx, y - 150)
        shader = _node(tree, "ShaderNodeAddShader", "add", sx + 220, y)
        tree.links.new(clear.outputs[0], shader.inputs[0])
        tree.links.new(glow.outputs[0], shader.inputs[1])
        method = "BLENDED"
    elif blend == "subtract":
        if isinstance(alpha, bpy.types.NodeSocket):
            colour = _multiply_colour(tree, colour, alpha, "darken by alpha", sx - 200, y + 200)
        invert = _node(tree, "ShaderNodeInvert", "subtract", sx, y)
        tree.links.new(colour, invert.inputs["Color"])
        shader = _node(tree, "ShaderNodeBsdfTransparent", "darken", sx + 220, y)
        tree.links.new(invert.outputs[0], shader.inputs["Color"])
        method = "BLENDED"
    else:
        if unlit:
            surface = _node(tree, "ShaderNodeEmission", "unlit", sx, y)
        else:
            surface = _node(tree, "ShaderNodeBsdfDiffuse", "lit", sx, y)
        tree.links.new(colour, surface.inputs["Color"])
        if flags.get("fog") != 0:
            # the fog over what is drawn, lit or not: its colour by the distance
            fog = _node(tree, "ShaderNodeGroup", "fog", sx - 200, y - 300)
            fog.node_tree = fog_group()
            tree.links.new(colour, fog.inputs[0])
            haze = _node(tree, "ShaderNodeEmission", "fog colour", sx, y - 300)
            tree.links.new(fog.outputs["Fog"], haze.inputs["Color"])
            fogged = _node(tree, "ShaderNodeMixShader", "fogged", sx + 110, y - 150)
            tree.links.new(fog.outputs["Factor"], fogged.inputs["Fac"])
            tree.links.new(surface.outputs[0], fogged.inputs[1])
            tree.links.new(haze.outputs[0], fogged.inputs[2])
            surface = fogged
        if blend == "opaque" or not isinstance(alpha, bpy.types.NodeSocket):
            shader = surface
        else:
            clear = _node(tree, "ShaderNodeBsdfTransparent", "behind", sx, y - 150)
            shader = _node(tree, "ShaderNodeMixShader", "coverage", sx + 220, y)
            tree.links.new(alpha, shader.inputs["Fac"])
            tree.links.new(clear.outputs[0], shader.inputs[1])
            tree.links.new(surface.outputs[0], shader.inputs[2])
        method = "DITHERED"
    shader.name = SHADER
    tree.links.new(shader.outputs[0], output.inputs["Surface"])
    if hasattr(material, "surface_render_method"):
        material.surface_render_method = method
    else:
        material.blend_method = "BLEND" if method == "BLENDED" else "HASHED"
    if hasattr(material, "use_transparency_overlap"):
        material.use_transparency_overlap = False
    material[FLAG] = 1
    return True


def apply_all(materials=None):
    count = 0
    for material in materials if materials is not None else bpy.data.materials:
        if is_map_material(material) and apply(material):
            count += 1
    return count


def export_materials(swapped):
    for material in bpy.data.materials:
        tree = material.node_tree
        if tree is None or not material.get(FLAG):
            continue
        output, principled, shader = _output(tree), _first(tree, "BSDF_PRINCIPLED"), tree.nodes.get(SHADER)
        if output is None or principled is None or shader is None:
            continue
        tree.links.new(principled.outputs[0], output.inputs["Surface"])
        swapped.append((material.name, shader.name))


def restore_materials(swapped):
    for name, shader_name in swapped:
        material = bpy.data.materials.get(name)
        if material is None:
            continue
        tree = material.node_tree
        output, shader = _output(tree), tree.nodes.get(shader_name)
        if output is not None and shader is not None:
            tree.links.new(shader.outputs[0], output.inputs["Surface"])


def shown_at(time, hour):
    try:
        start, end = int(time[0]), int(time[1])
    except (TypeError, ValueError, IndexError, KeyError):
        return True
    if start == end:
        return True
    if start < end:
        return start <= hour < end
    return hour >= start or hour < end


def shown(obj):
    value = c2data.to_py(c2data.lookup(obj, ("show",)))
    try:
        return value is None or int(value) != 0
    except (TypeError, ValueError):
        return True


def sky_band(root, hour):
    """The sky band the game draws most of at the hour (CMapSky::DrawSky weighs the four by
    GetLightingRatio, by the time of day the map has)."""
    follow, _blend, fixed, fixed_on = _time_flag(c2data.map_settings(root))
    if not follow:
        hour = fixed if fixed_on else 12.0
    elif hour is None:
        hour = 12.0
    ratios = band_ratios(4, float(hour))
    return max(range(4), key=lambda band: ratios[band])


def show_sky(context, hour, roots=None):
    """Shows the sky band of the hour, hides the others; the sky can be neither selected nor
    rendered over the map's own lights."""
    layer_objects = context.view_layer.objects
    for root in roots if roots is not None else c2data.scene_map_roots(context.scene):
        wanted = sky_band(root, hour)
        children = c2data.child_map()
        for obj in c2data.display_objects(root):
            obj.hide_select = True
            band = c2data.lookup(obj, ("band",))
            if c2data.lookup(obj, ("what",)) != "sky band":
                continue
            visible = band == wanted
            for below in c2data.descendants(obj, children):
                if below.name in layer_objects:
                    below.hide_set(not visible)
                below.hide_render = not visible


def show_hour(context, hour, enabled=True, roots=None):
    hidden = 0
    layer_objects = context.view_layer.objects
    show_sky(context, hour if enabled else None, roots)
    children = c2data.child_map()
    for root in roots if roots is not None else c2data.scene_map_roots(context.scene):
        for piece in c2data.descendants(root, children):
            if c2data.role(piece) != "piece":
                continue
            placement = c2data.placement_of(piece)
            visible = not enabled or (
                shown_at(c2data.to_py(c2data.lookup(piece, ("time",))), hour)
                and shown(piece)
                and (placement is None or shown(placement))
            )
            hidden += not visible
            for obj in c2data.descendants(piece, children):
                if obj.name in layer_objects:
                    obj.hide_set(not visible)
                obj.hide_render = not visible
    return hidden


def _srgb(value):
    value = max(0.0, min(1.0, float(value) / 255.0))
    return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4


def light_set_of(settings, number):
    """LIGHT_SET number of a map's settings (CMapInfo::GetLightingInfo): the set of that index, else
    the one at that place."""
    sets = [light_set for light_set in settings.get("light_sets") or [] if hasattr(light_set, "get")]
    for light_set in sets:
        if light_set.get("index") == number:
            return light_set
    return sets[number] if isinstance(number, int) and 0 <= number < len(sets) else None


def active_light_set(root):
    settings = c2data.map_settings(root)
    found = light_set_of(settings, settings.get("active_light_set"))
    if found is not None:
        return found
    sets = settings.get("light_sets") or []
    return sets[0] if sets and hasattr(sets[0], "get") else None


def _time_flag(settings):
    flag = settings.get("time_flag")
    if hasattr(flag, "get"):
        return (
            int(flag.get("follow_clock") or 0), int(flag.get("blend") or 0),
            float(flag.get("fixed_time") or 12.0), int(flag.get("fixed_time_enable") or 0),
        )
    return 0, 0, 12.0, 0


def _number(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def time_band(hour):
    """GetTimeBand (mapload.cpp:260): day 9-17 is band 0, evening 17-21 band 1, night band 2,
    morning 6-9 band 3; the four light sets of TIME_LIGHT_NUM 4 follow them."""
    if 6.0 <= hour < 9.0:
        return 3
    if 9.0 <= hour < 17.0:
        return 0
    if 17.0 <= hour < 21.0:
        return 1
    return 2


def band_ratios(count, hour):
    """How much each of `count` light sets weighs at the hour (GetTimeLightingRatio and
    GetLightingRatio, mapload.cpp:315-431): a set's band, blended into the next over the hour
    before the next starts."""
    ratios = [0.0] * count
    if count < 2:
        if count:
            ratios[0] = 1.0
        return ratios
    if count == 4:
        band = time_band(hour)
        start_blend = {3: 8.0, 0: 16.0, 1: 20.0}
        blend = 0.0
        if band in start_blend and hour > start_blend[band]:
            blend = hour - start_blend[band]
        elif band == 2 and 5.0 < hour < 6.0:
            blend = hour - 5.0
        ratios[band] = 1.0 - blend
        ratios[(band + 1) % 4] = blend
        return ratios
    length = 24.0 / count
    band = int(((hour - 9.0) % 24.0) / length) % count
    start = 9.0 + band * length
    if start >= 24.0:
        start -= 24.0
    left = start + length - hour
    blend = left if left < 1.0 else 1.0
    ratios[band] = blend
    ratios[(band + 1) % count] = 1.0 - blend
    return ratios


def _sun(hour, sun_angle):
    """Where the sun is at the hour (CMap::GetSunPoint, mapload.cpp:433), as a direction towards
    it, never lower than 0.2 up."""
    import math

    from mathutils import Matrix, Vector

    turn = Matrix.Rotation(sun_angle, 3, "Y") @ Matrix.Rotation(math.fmod(hour * 2.0 * math.pi / 24.0, 2.0 * math.pi), 3, "Z")
    sun = (turn @ Vector((0.0, -1900.0, 700.0))).normalized()
    if sun.y < 0.2:
        sun.y = 0.2
        sun.normalize()
    return [sun.x, sun.y, sun.z]


def _plain(light_set):
    lights = []
    for light in light_set.get("lights") or []:
        if hasattr(light, "get") and light.get("type") == "light":
            lights.append({"index": light.get("index", len(lights)), "dir": list(light.get("dir") or [0, 1, 0]),
                           "color": list(light.get("color") or [128, 128, 128])})
    fog = light_set.get("fog")
    if not hasattr(fog, "get"):
        fog = {}
    return {
        "bgcolor": list(light_set.get("bgcolor") or [0, 0, 0]),
        "ambient": list(light_set.get("ambient") or [64, 64, 64]),
        "lights": lights,
        "fog_enable": int(light_set.get("fog_enable") or 0),
        "fog": {"near": float(fog.get("near") or 0.0), "far": float(fog.get("far") or 0.0),
                "color": list(fog.get("color") or [0, 0, 0])},
    }


def lighting_at(root, hour=None):
    """The lighting the game uses (CMap::GetLightInfo, mapload.cpp:482): the active light set
    unless the map follows the clock or a fixed hour; then the band's set at the hour, blended
    with the next (and the first light following the sun) when TIME_FLAG blends. `hour` None is
    the start of the game's day as the map has it (noon unless a fixed hour)."""
    settings = c2data.map_settings(root)
    follow, blend, fixed, fixed_on = _time_flag(settings)
    count = _number(settings.get("time_light_num"), 0)
    active = _number(settings.get("active_light_set"), 0)
    if active >= count or (not follow and not fixed_on):
        found = light_set_of(settings, active) or active_light_set(root)
        return _plain(found) if found is not None else None
    # GetNowTime: the game's clock when the map follows it, else its fixed hour, else noon
    if follow:
        hour = 12.0 if hour is None else float(hour)
    else:
        hour = fixed if fixed_on else 12.0
    sets = [light_set_of(settings, number) for number in range(count)]
    if any(found is None for found in sets):
        return None
    band = time_band(hour) if count == 4 else (int(((hour - 9.0) % 24.0) / (24.0 / count)) % count if count >= 2 else 0)
    out = _plain(sets[band])
    if not blend:
        return out
    ratios = band_ratios(count, hour)
    plain = [_plain(found) for found in sets]
    weigh = lambda key: [sum(r * float(p[key][i]) for r, p in zip(ratios, plain) if r > 0.0) for i in range(3)]
    out["bgcolor"], out["ambient"] = weigh("bgcolor"), weigh("ambient")
    lights = []
    for slot in range(4):
        direction, colour, any_light = [0.0] * 3, [0.0] * 3, False
        for ratio, p in zip(ratios, plain):
            if ratio <= 0.0 or slot >= len(p["lights"]):
                continue
            any_light = True
            for axis in range(3):
                direction[axis] += ratio * float(p["lights"][slot]["dir"][axis])
                colour[axis] += ratio * float(p["lights"][slot]["color"][axis])
        if any_light:
            lights.append({"index": slot, "dir": direction, "color": colour})
    fogged = [(r, p) for r, p in zip(ratios, plain) if r > 0.0 and p["fog_enable"]]
    out["fog_enable"] = int(bool(fogged))
    out["fog"] = {
        "near": sum(r * p["fog"]["near"] for r, p in fogged), "far": sum(r * p["fog"]["far"] for r, p in fogged),
        "color": [sum(r * float(p["fog"]["color"][i]) for r, p in fogged) for i in range(3)],
    }
    if lights:
        sky = settings.get("sky_info")
        angle = float(sky.get("sun_angle") or 0.0) if hasattr(sky, "get") else 0.0
        import math

        lights[0]["dir"] = _sun(hour, math.radians(angle))
    out["lights"] = lights
    return out


def update_time_of_day(context):
    """Lights every map of the scene as the game does at the Map panel's hour."""
    settings = getattr(context.window_manager, "dc2tk_maps", None)
    if settings is None:
        return
    for root in c2data.scene_map_roots(context.scene):
        map_world(context, root, lighting_at(root, settings.hour if settings.by_hour else None))


def map_world(context, root, lighting=None):
    from mathutils import Vector

    light_set = lighting if lighting is not None else lighting_at(root)
    if light_set is None:
        return None
    name = "c2 %s" % c2data.map_name(root)
    scene = context.scene
    world = bpy.data.worlds.get(name) or bpy.data.worlds.new(name)
    if bpy.app.version < (5, 0, 0):
        world.use_nodes = True
    tree = world.node_tree
    tree.nodes.clear()
    output = tree.nodes.new("ShaderNodeOutputWorld")
    output.location = (400, 0)
    seen = tree.nodes.new("ShaderNodeBackground")
    seen.label = "background (BGCOLOR)"
    seen.location = (0, 100)
    lit = tree.nodes.new("ShaderNodeBackground")
    lit.label = "ambient (AMBIENT)"
    lit.location = (0, -100)
    path = tree.nodes.new("ShaderNodeLightPath")
    path.location = (-250, 250)
    mix = tree.nodes.new("ShaderNodeMixShader")
    mix.location = (200, 0)
    background = light_set.get("bgcolor") or [0, 0, 0]
    seen.inputs["Color"].default_value = [_srgb(c) for c in background[:3]] + [1.0]
    ambient = light_set.get("ambient") or [64, 64, 64]
    lit.inputs["Color"].default_value = [max(0.0, float(c)) / 128.0 for c in ambient[:3]] + [1.0]
    tree.links.new(path.outputs["Is Camera Ray"], mix.inputs["Fac"])
    tree.links.new(lit.outputs[0], mix.inputs[1])
    tree.links.new(seen.outputs[0], mix.inputs[2])
    tree.links.new(mix.outputs[0], output.inputs["Surface"])
    if scene.world is None or scene.world.name in ("World", name) or scene.world.name.startswith("c2 "):
        scene.world = world

    prefix = SUN_PREFIX % c2data.map_name(root)
    existing = {obj.name: obj for obj in bpy.data.objects if obj.name.startswith(prefix)}
    made = set()
    for at, light in enumerate(light_set.get("lights") or []):
        if not hasattr(light, "get") or light.get("type", "light") != "light":
            continue
        direction = light.get("dir") or [0, 1, 0]
        towards = Vector((float(direction[0]), -float(direction[2]), float(direction[1])))
        if towards.length == 0:
            continue
        colour = light.get("color") or [128, 128, 128]
        strength = max(float(c) for c in colour[:3]) / 128.0
        name = prefix + str(at)
        sun = existing.get(name)
        if sun is None or sun.type != "LIGHT":
            sun = bpy.data.objects.new(name, bpy.data.lights.new(name, "SUN"))
            scene.collection.objects.link(sun)
        sun.data.color = [max(0.0, float(c)) / max(1.0, max(float(c) for c in colour[:3])) for c in colour[:3]]
        sun.data.energy = 3.14159 * strength
        sun.rotation_mode = "XYZ"
        sun.rotation_euler = towards.normalized().to_track_quat("Z", "Y").to_euler()
        sun.location = root.location
        sun[SUN_KEY] = int(light.get("index", at))
        made.add(name)
    for name, obj in existing.items():
        if name not in made:
            data = obj.data
            bpy.data.objects.remove(obj)
            if data is not None and data.users == 0:
                bpy.data.lights.remove(data)
    settings = getattr(context.window_manager, "dc2tk_maps", None)
    set_fog(light_set, settings is None or settings.show_fog)
    return world


SUN_PREFIX = "c2 light %s "
SUN_KEY = "c2_light"
FOG = "c2 fog"


def fog_group():
    """The node group every map material passes its colour through: the light set's fog, by the
    distance from the view, linear from near to far (FOG near,far,r,g,b), its strength 0 when the
    set has none or the preview is off."""
    group = bpy.data.node_groups.get(FOG)
    if group is not None:
        return group
    group = bpy.data.node_groups.new(FOG, "ShaderNodeTree")
    sockets = (("INPUT", "NodeSocketColor", "Color"), ("OUTPUT", "NodeSocketColor", "Color"),
               ("OUTPUT", "NodeSocketFloat", "Factor"), ("OUTPUT", "NodeSocketColor", "Fog"))
    for side, kind, label in sockets:
        if hasattr(group, "interface"):
            group.interface.new_socket(label, in_out=side, socket_type=kind)
        else:
            (group.inputs if side == "INPUT" else group.outputs).new(kind, label)
    nodes, links = group.nodes, group.links
    given = nodes.new("NodeGroupInput")
    out = nodes.new("NodeGroupOutput")
    camera = nodes.new("ShaderNodeCameraData")
    ramp = nodes.new("ShaderNodeMapRange")
    ramp.name = "range"
    ramp.clamp = True
    strength = nodes.new("ShaderNodeMath")
    strength.name = "strength"
    strength.operation = "MULTIPLY"
    colour = nodes.new("ShaderNodeRGB")
    colour.name = "colour"
    mix = nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    links.new(camera.outputs["View Distance"], ramp.inputs["Value"])
    links.new(ramp.outputs["Result"], strength.inputs[0])
    links.new(strength.outputs[0], mix.inputs["Factor"])
    sockets = [s for s in mix.inputs if s.type == "RGBA"]
    links.new(given.outputs[0], sockets[0])
    links.new(colour.outputs[0], sockets[1])
    links.new(next(s for s in mix.outputs if s.type == "RGBA"), out.inputs[0])
    links.new(strength.outputs[0], out.inputs[1])
    links.new(colour.outputs[0], out.inputs[2])
    strength.inputs[1].default_value = 0.0
    return group


def set_fog(lighting, shown=True):
    group = fog_group()
    fog = lighting.get("fog") if hasattr(lighting.get("fog"), "get") else {}
    near, far = float(fog.get("near") or 0.0), float(fog.get("far") or 0.0)
    on = bool(lighting.get("fog_enable")) and shown and far > near
    ramp = group.nodes["range"]
    ramp.inputs["From Min"].default_value = near
    ramp.inputs["From Max"].default_value = max(far, near + 1.0)
    group.nodes["strength"].inputs[1].default_value = 1.0 if on else 0.0
    rgb = fog.get("color") or [0, 0, 0]
    group.nodes["colour"].outputs[0].default_value = [_srgb(c) for c in list(rgb)[:3]] + [1.0]
    return on
