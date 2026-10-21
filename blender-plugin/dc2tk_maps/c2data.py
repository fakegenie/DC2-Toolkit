import ast

import bpy

from . import schema

KEY = "c2"


def to_py(value):
    if hasattr(value, "to_dict") and hasattr(value, "keys"):
        return {key: to_py(value[key]) for key in value.keys()}
    if hasattr(value, "to_list"):
        return [to_py(item) for item in value.to_list()]
    if isinstance(value, (list, tuple)):
        return [to_py(item) for item in value]
    return value


def recovered(text):
    try:
        value = ast.literal_eval(text)
    except (ValueError, SyntaxError):
        return None
    return value if isinstance(value, dict) else None


def get(obj):
    if obj is None:
        return None
    value = obj.get(KEY)
    if value is None:
        return None
    if isinstance(value, str):
        return recovered(value)
    value = to_py(value)
    return value if isinstance(value, dict) else None


def is_stringified(obj):
    return obj is not None and isinstance(obj.get(KEY), str)


def put(obj, data):
    obj[KEY] = data

    from . import outliner, overlay

    # a property's change is no depsgraph update: what is worked out from the map is out of date
    overlay.invalidate()
    outliner.invalidate()


def rename(obj, name):
    if obj is None or not name:
        return
    prefix = schema.PREFIX[role(obj)]
    assign(obj, ("name",), name)
    obj.name = prefix + name


def map_settings(root):
    settings = to_py(lookup(root, ("map", "settings")))
    return settings if isinstance(settings, dict) else {}


def role(obj):
    data = obj.get(KEY) if obj is not None else None
    if data is None:
        return None
    if isinstance(data, str):
        data = recovered(data) or {}
    try:
        return data.get("role")
    except AttributeError:
        return None


def lookup(obj, path, default=None):
    current = obj.get(KEY) if obj is not None else None
    if isinstance(current, str):
        current = recovered(current)
    for step in path:
        if current is None:
            return default
        try:
            if isinstance(step, int):
                current = current[step]
            else:
                current = current.get(step) if hasattr(current, "get") else None
        except (IndexError, KeyError, TypeError):
            return default
    return default if current is None else current


def _assign(container, step, value):
    if isinstance(step, int):
        while len(container) <= step:
            container.append(None)
        container[step] = value
    else:
        container[step] = value


def assign(obj, path, value):
    data = get(obj) or {}
    current = data
    for index, step in enumerate(path[:-1]):
        following = path[index + 1]
        try:
            child = current[step]
        except (KeyError, IndexError, TypeError):
            child = None
        if not isinstance(child, (dict, list)):
            child = [] if isinstance(following, int) else {}
            _assign(current, step, child)
        current = child
    _assign(current, path[-1], value)
    put(obj, data)


def remove(obj, path):
    data = get(obj)
    if data is None:
        return
    current = data
    for step in path[:-1]:
        current = current[step]
    if isinstance(current, dict):
        current.pop(path[-1], None)
    else:
        del current[path[-1]]
    put(obj, data)


def map_root(obj):
    while obj is not None:
        if role(obj) == "map" or (obj.name.startswith(schema.PREFIX["map"]) and obj.parent is None):
            return obj
        obj = obj.parent
    return None


def scene_map_roots(scene=None):
    scene = scene or bpy.context.scene
    return [obj for obj in scene.objects if obj.parent is None and (role(obj) == "map" or obj.name.startswith("map:"))]


def current_map_root(context=None):
    context = context or bpy.context
    found = map_root(getattr(context, "active_object", None))
    if found is not None:
        return found
    roots = scene_map_roots(context.scene)
    return roots[0] if roots else None


def map_name(root):
    return schema.game_name(root.name, "map") if root is not None else ""


def child_map():
    found = {}
    for obj in bpy.data.objects:
        if obj.parent is not None:
            found.setdefault(obj.parent, []).append(obj)
    return found


def descendants(obj, children=None):
    children = child_map() if children is None else children
    found = []
    stack = [obj]
    while stack:
        current = stack.pop()
        found.append(current)
        stack.extend(children.get(current, ()))
    return found


def make_new_node(obj):
    """Makes a copied model node a new one: no mds_index, in its extras.c2 or beside it (Blender
    keeps the glTF node's extras.mds_index as a property of its own), so the build gives it a
    frame of its own instead of the one it was copied from."""
    put(obj, {"role": "mds"})
    if "mds_index" in obj:
        del obj["mds_index"]


def map_objects(root):
    """The map's own objects: every one under its root but what the scene only shows (role
    display: its sky), which is never exported."""
    children = child_map()
    found = []
    stack = [root]
    while stack:
        current = stack.pop()
        if role(current) == "display":
            continue
        found.append(current)
        stack.extend(children.get(current, ()))
    return found


def display_objects(root):
    """What the scene only shows of the map (its sky): every display node and what is below."""
    children = child_map()
    found = []
    for obj in descendants(root, children):
        if role(obj) == "display" and (obj.parent is None or role(obj.parent) != "display"):
            found.extend(descendants(obj, children))
    return found


def group_node(root, name):
    for child in root.children:
        if schema.strip_suffix(child.name) == name:
            return child
    return None


def placement_of(obj):
    while obj is not None:
        if role(obj) == "placement":
            return obj
        obj = obj.parent
    return None


def piece_of(obj):
    while obj is not None:
        if role(obj) == "piece":
            return obj
        obj = obj.parent
    return None


def placements(root):
    return [obj for obj in descendants(root) if role(obj) == "placement"]


def points(root):
    return [obj for obj in descendants(root) if role(obj) == "point"]


def collision_pieces(root, kinds=schema.COLLISION_KINDS):
    return [
        obj
        for obj in descendants(root)
        if role(obj) == "piece" and lookup(obj, ("kind",)) in kinds
    ]


def collision_meshes(root):
    children = child_map()
    found = []
    for piece in collision_pieces(root):
        for obj in descendants(piece, children):
            if obj.type == "MESH":
                found.append(obj)
    return found


def is_collision_mesh(obj):
    piece = piece_of(obj)
    return obj.type == "MESH" and piece is not None and lookup(piece, ("kind",)) in schema.COLLISION_KINDS


def data_field(obj, field):
    data = lookup(obj, ("data",))
    value = data.get(field["key"]) if hasattr(data, "keys") else None
    if value is None:
        return field["default"]
    if field["size"] > 1:
        value = list(to_py(value))
        value += list(field["default"])[len(value):]
        return value[: field["size"]]
    return value


def set_data_field(obj, field, value):
    assign(obj, ("data", field["key"]), list(value) if field["size"] > 1 else value)


def exact_name(obj, role=None):
    name = lookup(obj, ("name",))
    return name if isinstance(name, str) else schema.game_name(obj.name, role)


def frame_base(name):
    return name.split("--", 1)[0]


def placements_by_name(root):
    found = {}
    if root is None:
        return found
    group = group_node(root, "placements")
    candidates = group.children if group is not None else placements(root)
    for obj in candidates:
        if role(obj) != "placement":
            continue
        found.setdefault(exact_name(obj, "placement"), obj)
    return found


def world_matrix(obj):
    """obj's world matrix from its parents' own, so right for objects the depsgraph has not
    evaluated (those in hidden collections)."""
    matrix = obj.matrix_basis.copy()
    while obj.parent is not None:
        matrix = obj.matrix_parent_inverse @ matrix
        obj = obj.parent
        matrix = obj.matrix_basis @ matrix
    return matrix


def is_preview(placement):
    return bool(lookup(placement, ("preview",)))


def template_name(placement):
    return str(lookup(placement, ("template",), ""))


def instances(placement):
    """Every placement of the map placing the same template as `placement` (itself first); a
    template library's preview is its template's only one."""
    if placement is None:
        return []
    if is_preview(placement):
        return [placement]
    root = map_root(placement)
    wanted = template_name(placement)
    others = [
        obj for obj in placements(root)
        if obj is not placement and not is_preview(obj) and template_name(obj) == wanted
    ] if root is not None else []
    return [placement] + others


def contents(placement):
    """A placement's own copy of its template, as the build compares placements: its pieces and
    points in order, each with its transform in the placement and its values."""
    found = []
    for child in placement.children:
        if role(child) not in ("piece", "point"):
            continue
        data = get(child) or {}
        local = [round(value, 3) for row in (child.matrix_parent_inverse @ child.matrix_basis) for value in row]
        found.append((role(child), exact_name(child, role(child)), local, repr(sorted(data.items()))))
    found.sort(key=lambda item: (item[0], item[1]))
    return found


def instance_groups(placement):
    """The placements of `placement`'s template grouped by their contents, `placement`'s group
    first: one group means the build changes the template, more means each other group gets a
    template of its own."""
    groups = []
    for obj in instances(placement):
        key = contents(obj)
        for group in groups:
            if group[0] == key:
                group[1].append(obj)
                break
        else:
            groups.append((key, [obj]))
    return [objects for _key, objects in groups]


def template_names(root):
    """Every template name the map's placements give."""
    return {template_name(obj) for obj in placements(root)} if root is not None else set()


def model_nodes(piece):
    found = {}
    below = piece.children_recursive if hasattr(piece, "children_recursive") else descendants(piece)[1:]
    nodes = [node for node in below if role(node) == "mds"]
    nodes.sort(key=lambda node: (lookup(node, ("mds_index",), 1 << 30), node.name))
    for node in nodes:
        found.setdefault(frame_base(exact_name(node)), node)
    return found


def anime_target(obj, by_name=None, nodes_of=None):
    fields = {field["key"]: field for field in schema.DATA_FIELDS["anime"]}
    placement = placement_of(obj)
    if placement is None:
        name = data_field(obj, fields["placement"])
        if by_name is None:
            by_name = placements_by_name(map_root(obj))
        placement = by_name.get(name)
        if placement is None:
            return None, "no placement %r" % name
    piece_name = data_field(obj, fields["piece"])
    if not piece_name:
        return placement, None
    piece = next(
        (child for child in placement.children if role(child) == "piece" and exact_name(child, "piece") == piece_name),
        None,
    )
    if piece is None:
        return None, "no piece %r in %s" % (piece_name, placement.name)
    frame_name = data_field(obj, fields["frame"])
    if not frame_name:
        return piece, None
    if nodes_of is None:
        nodes = model_nodes(piece)
    else:
        nodes = nodes_of.get(piece.name)
        if nodes is None:
            nodes = nodes_of[piece.name] = model_nodes(piece)
    node = nodes.get(frame_base(frame_name))
    if node is None:
        return None, "no model node %r in %s" % (frame_name, piece.name)
    return node, None
