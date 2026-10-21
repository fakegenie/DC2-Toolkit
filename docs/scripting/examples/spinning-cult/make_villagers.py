"""Gives five Palm Brinks townspeople of The Spinning Cult two new motions, made in Blender as one
would by hand, in their own character packs:

- `spin`: the person has been turned: arms hanging limp at the sides, head tipped back and a little
  to one side as if staring up at something, and the whole body turning slowly on the spot about
  the vertical through the middle of the feet, one full turn in 3 seconds, swaying very slightly;
- `stare`: standing utterly still, arms limp, head lowered a little and tilted: a held pose that
  loops every 2 seconds, for the ending where they all stand facing a door.

The packs: Parn (chara/p01_10a.chr), Sheriff Blinkhorn (p01_05a), Giles (p01_19a), Aegira (p01_20a)
and Fournet (p01_22a). Each is exported from your own disc into the mod as glTF:

    dc2tk mod asset pal.iso spinning-cult chara/p01_19a.chr --as gltf

which writes mods/spinning-cult/raw/chara/p01_19a.chr.gltf/ (p01_19a.gltf, .bin, textures/) and
adds its line to mod.json's "files" (`"chara/p01_19a.chr": "raw/chara/p01_19a.chr.gltf"`), keeping
the lines the mod has, and refusing to run while one of them names a file the mod's folder does not
have yet. This script opens each export in
Blender, finds the bones by looking at the skeleton (their names differ from pack to pack), adds
the two actions and exports glTF Separate over the export, so `dc2tk build` adds KEYs `spin` and
`stare` after the pack's last and changes nothing else in the pack.

Every key list of a pack gets a key at every frame of every KEY, so a motion costs the pack about
32 bytes a frame for each of its 30 to 40 lists, and these packs load into the villagers' memory
stack. So the motions are short and played slowly, as the game's own are: `spin` is 27 frames at
step 0.30 (27 / (30 x 0.30) = 3 s; its last frame is its first, a whole turn on, so the loop is
seamless; the engine slerps a turn of 13.3 degrees between keys, which is exact for a turn about
one axis), and `stare` 3 frames at step 0.05 (2 s). The steps go into the animations' `extras.step`
after Blender's export, which build reads for a new KEY's step.

    blender --background --factory-startup --python docs/coding/examples/spinning-cult/make_villagers.py
    blender --background --factory-startup --python docs/coding/examples/spinning-cult/make_villagers.py -- DC2TK ISO MODS

DC2TK, ISO and MODS default to build/release/dc2tk.exe, pal.iso and mods/ in the repository; the mod is
MODS/spinning-cult. Nothing of the game's is in this folder: the models come from your own disc.
"""

import json
import math
import os
import subprocess
import sys

import bpy
from mathutils import Matrix, Quaternion, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(HERE))))
MOD = "spinning-cult"

PACKS = [
    ("p01_10a", "Parn"),
    ("p01_05a", "Sheriff Blinkhorn"),
    ("p01_19a", "Giles"),
    ("p01_20a", "Aegira"),
    ("p01_22a", "Fournet"),
]

FPS = 30  # motion frames a second at step 1.00
SPIN_FRAMES, SPIN_STEP = 27, 0.30  # one turn in 27 / (30 x 0.30) = 3 s; frame 27 is frame 0 again
STARE_FRAMES, STARE_STEP = 3, 0.05  # 3 / (30 x 0.05) = 2 s
SWAY_DEGREES = 1.5  # the spinning body's lean from side to side, twice a turn

# The limp arm, in the body's frame (lateral = out from the body's side, forward, up): the upper
# arm hangs straight down, a little out from the body; the forearm a little forward of it, an
# elbow bent a few degrees; the hand goes on in the forearm's line.
UPPER_ARM = (0.10, 0.02, -1.0)
FOREARM = (0.04, 0.16, -1.0)
# Where something held hangs (Aegira's and Fournet's umbrellas): down and forward.
HELD = (0.2, 0.35, -1.0)

# How the head is held (degrees): (the face's pitch above the level, roll to the side).
SPIN_HEAD = (42.0, 18.0)
STARE_HEAD = (-20.0, -11.0)
NECK_SHARE = 0.3  # of the turn that brings the face there
# How far below the line of its head chain (joint to effector) a pack's face is modelled, where it
# is not along it: Parn's hunched face looks some 25 degrees lower.
FACE_BELOW = {"p01_10a": 25.0}


def arguments():
    given = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    dc2tk = given[0] if len(given) > 0 else os.path.join(REPO, "build", "release", "dc2tk.exe")
    iso = given[1] if len(given) > 1 else os.path.join(REPO, "pal.iso")
    mods = given[2] if len(given) > 2 else os.path.join(REPO, "mods")
    return os.path.abspath(dc2tk), os.path.abspath(iso), os.path.abspath(mods)


def export_packs(dc2tk, iso, mods):
    """Each pack as glTF into the mod's raw/ folder, fresh from the disc."""
    paths = ["chara/%s.chr" % name for name, _ in PACKS]
    subprocess.run([dc2tk, "mod", "asset", iso, MOD] + paths + ["--as", "gltf", "--mods", mods], check=True)
    return {name: os.path.join(mods, MOD, "raw", "chara", name + ".chr.gltf", name + ".gltf") for name, _ in PACKS}


# --- the skeleton ------------------------------------------------------------------------------


class Skeleton:
    """The glTF's node tree (names and parents) and the bones the motions move, found by name and
    place. The packs were rigged in Softimage: chains of joints `jnt..`, each chain from a `chn..`
    to an effector `eff..`; a limb is a chain of two joints (`jnt18_1`, its child `jnt18_2`)."""

    def __init__(self, path, pose):
        with open(path, encoding="utf-8") as f:
            gltf = json.load(f)
        nodes = gltf["nodes"]
        self.names = [n.get("name", "") for n in nodes]
        self.parent = {}
        for i, n in enumerate(nodes):
            for c in n.get("children", []):
                self.parent[c] = i
        self.children = {i: n.get("children", []) for i, n in enumerate(nodes)}
        self.pose = pose  # name -> position in the stand pose (armature space, Z up)
        joint = lambda i: self.names[i].lower().startswith("jnt")

        # the head: the frame the face mesh (`kao`) hangs on, else the highest joint
        faces = [i for i, n in enumerate(self.names) if n.lower() == "kao" and i in self.parent]
        if faces:
            self.head = self.parent[faces[0]]
        else:
            self.head = max((i for i in range(len(nodes)) if joint(i)), key=lambda i: self.z(i))
        # the path from the root to the head; the body: the first node on it well off the ground
        path = [self.head]
        while path[-1] in self.parent:
            path.append(self.parent[path[-1]])
        path.reverse()
        self.root = path[0]
        height = self.z(self.head)
        self.body = next(i for i in path if self.z(i) > 0.3 * height)
        # the neck: the joint above the head's on the path
        above = [i for i in path[:-1] if joint(i) and i != self.head]
        self.neck = above[-1] if above else None
        self.spine = [i for i in above[:-1] if path.index(i) > path.index(self.body)]
        # where the face looks: the head chain's effector, which sits in front of the face
        ends = [e for e in self.children[self.head] if self.names[e].lower().startswith("eff")]
        self.gaze = ends[0] if ends else None

        # limbs: a joint whose child is a joint, the upper one's end (the effector) under the lower
        limbs = []
        for i in range(len(nodes)):
            if not joint(i):
                continue
            for c in self.children[i]:
                if joint(c):
                    ends = [e for e in self.children[c] if self.names[e].lower().startswith("eff")]
                    if ends:
                        limbs.append((i, c, ends[0]))
        body_z = self.z(self.body)
        arms = [l for l in limbs if self.z(l[0]) > body_z + 0.25 * (height - body_z)]
        legs = [l for l in limbs if self.z(l[0]) <= body_z + 0.05 * height]
        if len(arms) != 2 or not legs:
            raise RuntimeError("%s: found %d arms and %d legs" % (path, len(arms), len(legs)))
        # sides: left is +x (the characters face -y in Blender, +z in the glTF)
        arms.sort(key=lambda l: self.at(l[0]).x)
        self.arms = {}
        for side, (upper, lower, wrist) in zip(("right", "left"), arms):
            hand = self.first_joint_under(wrist)
            hand_end = None
            if hand is not None:
                ends = [e for e in self.children[hand] if self.names[e].lower().startswith("eff")]
                hand_end = ends[0] if ends else None
            self.arms[side] = (upper, lower, wrist, hand, hand_end)
        self.ankles = [l[2] for l in legs]  # the legs: only to check the skeleton reads as a body

    def at(self, i):
        return self.pose[self.names[i]]

    def z(self, i):
        return self.at(i).z

    def first_joint_under(self, i):
        todo = list(self.children[i])
        while todo:
            n = todo.pop(0)
            if self.names[n].lower().startswith("jnt"):
                return n
            todo.extend(self.children[n])
        return None

    def describe(self):
        n = self.names
        arm = lambda a: "%s > %s > %s, hand %s" % (n[a[0]], n[a[1]], n[a[2]], n[a[3]] if a[3] is not None else "-")
        return "body %s, spine %s, neck %s, head %s; left arm %s; right arm %s; ankles %s" % (
            n[self.body], [n[i] for i in self.spine], n[self.neck] if self.neck is not None else "-",
            n[self.head], arm(self.arms["left"]), arm(self.arms["right"]), [n[i] for i in self.ankles])


# --- posing ------------------------------------------------------------------------------------


def turn_about(matrix, point, rotation):
    """A bone's armature-space matrix turned by rotation (3x3) about point."""
    return Matrix.Translation(point) @ rotation.to_4x4() @ Matrix.Translation(-point) @ matrix


def swing(bones, name, start, end, direction):
    """Turns bone `name` about its head so the line from `start` to `end` (bones whose heads it
    joins) points along `direction`: the least turn, keeping the twist."""
    pb = bones[name]
    now = bones[end].head - bones[start].head
    if now.length < 1e-6:
        return
    q = now.normalized().rotation_difference(direction.normalized())
    pb.matrix = turn_about(pb.matrix.copy(), pb.head.copy(), q.to_matrix())
    bpy.context.view_layer.update()


def lean(bones, name, axis, degrees):
    """Turns bone `name` about its head by `degrees` about `axis` (armature space)."""
    pb = bones[name]
    r = Matrix.Rotation(math.radians(degrees), 3, axis)
    pb.matrix = turn_about(pb.matrix.copy(), pb.head.copy(), r)
    bpy.context.view_layer.update()


def held_props(rig, hand, reach):
    """The meshes a hand bone carries that reach further from it than `reach` (an umbrella; not
    the hand's own mesh), as their vertices in armature space."""
    names = {hand} | {b.name for b in rig.pose.bones[hand].children_recursive}
    to_rig = rig.matrix_world.inverted()
    origin = rig.pose.bones[hand].head
    props = []
    for o in bpy.data.objects:
        if o.type != 'MESH' or o.parent != rig or o.parent_type != 'BONE' or o.parent_bone not in names:
            continue
        m = to_rig @ o.matrix_world
        vertices = [m @ v.co for v in o.data.vertices]
        if vertices and max((v - origin).length for v in vertices) > reach:
            props.append(vertices)
    return props


def let_hang(bones, rig, hand, hand_end, reach, target):
    """Turns a hand that holds something long (Aegira's and Fournet's umbrellas) so it hangs along
    `target` instead of sticking out where the stand pose held it: first about the hand's own line,
    then the rest of the way at the wrist."""
    pb = bones[hand]

    def held():
        props = held_props(rig, hand, reach)
        if not props:
            return None
        vertices = max(props, key=len)
        return (sum(vertices, Vector()) / len(vertices) - pb.head).normalized()

    d = held()
    if d is None:
        return
    target = target.normalized()
    axis = (bones[hand_end].head - pb.head).normalized()
    d_flat = d - axis * d.dot(axis)
    t_flat = target - axis * target.dot(axis)
    if d_flat.length > 1e-6 and t_flat.length > 1e-6:
        d_flat.normalize()
        t_flat.normalize()
        lean(bones, hand, axis, math.degrees(math.atan2(d_flat.cross(t_flat).dot(axis), d_flat.dot(t_flat))))
    turn = held().rotation_difference(target)
    pb.matrix = turn_about(pb.matrix.copy(), pb.head.copy(), turn.to_matrix())
    bpy.context.view_layer.update()


def feet_centre(rig, sk):
    """The middle of the soles in the pose as it stands (armature space, on the floor): the
    centre of the skinned body's vertices within a few percent of its height of the floor, as
    posed. Not the ankle joints: the shoes reach well in front of them. (Rigid meshes, such as an
    umbrella whose tip rests by the feet, are left out.)"""
    depsgraph = bpy.context.evaluated_depsgraph_get()
    to_rig = rig.matrix_world.inverted()
    points = []
    for o in bpy.context.scene.objects:
        # the character's skinned meshes, not the bone shapes the importer makes
        if o.type != 'MESH' or not any(m.type == 'ARMATURE' and m.object == rig for m in o.modifiers):
            continue
        e = o.evaluated_get(depsgraph)
        mesh = e.to_mesh()
        m = to_rig @ e.matrix_world
        points.extend(m @ v.co for v in mesh.vertices)
        e.to_mesh_clear()
    floor = min(p.z for p in points)
    sole = floor + 0.03 * (sk.z(sk.head) - floor)
    low = [p for p in points if p.z <= sole]
    centre = sum(low, Vector()) / len(low)
    return Vector((centre.x, centre.y, floor))


def set_action(rig, action):
    rig.animation_data.action = action
    if action is not None and getattr(rig.animation_data, "action_slot", None) is None and len(action.slots):
        rig.animation_data.action_slot = action.slots[0]


def stand_pose(rig):
    """The pose bones set to the first frame of the pack's first KEY (立ち, standing), and left so
    with no action playing. Returns each bone's head (armature space)."""
    actions = [a for a in bpy.data.actions if not a.name.startswith(("spin", "stare"))]
    stand = next((a for a in actions if a.name == "立ち"), actions[0])
    set_action(rig, stand)
    bpy.context.scene.frame_set(int(stand.frame_range[0]))
    basis = {pb.name: pb.matrix_basis.copy() for pb in rig.pose.bones}
    set_action(rig, None)
    for pb in rig.pose.bones:
        pb.rotation_mode = 'QUATERNION'
        pb.matrix_basis = basis[pb.name]
    bpy.context.view_layer.update()
    return {pb.name: pb.head.copy() for pb in rig.pose.bones}


def body_axes(sk):
    """The body's lateral (towards its left), forward and up axes in the stand pose."""
    up = Vector((0, 0, 1))
    left = sk.at(sk.arms["left"][0]) - sk.at(sk.arms["right"][0])
    left.z = 0
    left.normalize()
    forward = left.cross(up)
    return left, forward, up


def turned_pose(rig, sk, head_angles, face_below=0.0):
    """Arms hanging limp and the head held at head_angles, over the stand pose."""
    bones = rig.pose.bones
    n = sk.names
    left, forward, up = body_axes(sk)
    for side, out in (("left", left), ("right", -left)):
        upper, lower, wrist, hand, hand_end = sk.arms[side]
        frame = lambda v: (out * v[0] + forward * v[1] + up * v[2])
        swing(bones, n[upper], n[upper], n[lower], frame(UPPER_ARM))
        swing(bones, n[lower], n[lower], n[wrist], frame(FOREARM))
        if hand is not None and hand_end is not None:
            swing(bones, n[hand], n[hand], n[hand_end], frame(FOREARM))
            reach = (bones[n[wrist]].head - bones[n[lower]].head).length
            let_hang(bones, rig, n[hand], n[hand_end], reach, frame(HELD))
    pitch, roll = head_angles
    if sk.gaze is not None:
        # the face turned to look straight ahead of the body, `pitch` above the level: the neck
        # takes a share of the turn, the head the rest (whatever way the stand pose looked)
        p = math.radians(pitch + face_below)
        target = forward * math.cos(p) + up * math.sin(p)
        gaze = lambda: (bones[n[sk.gaze]].head - bones[n[sk.head]].head).normalized()
        if sk.neck is not None:
            share = Quaternion().slerp(gaze().rotation_difference(target), NECK_SHARE)
            pb = bones[n[sk.neck]]
            pb.matrix = turn_about(pb.matrix.copy(), pb.head.copy(), share.to_matrix())
            bpy.context.view_layer.update()
        swing(bones, n[sk.head], n[sk.head], n[sk.gaze], target)
    else:
        lean(bones, n[sk.head], left, -pitch)  # about the lateral axis: up is back
    lean(bones, n[sk.head], forward, roll)
    return {pb.name: pb.matrix_basis.copy() for pb in bones}


def key_bones(rig, basis, frames, moving=None):
    """Keys every bone at basis over frames 0..frames: a still bone at the ends, `moving` (name ->
    function of the frame giving its basis) at every frame."""
    bones = rig.pose.bones
    moving = moving or {}
    for pb in bones:
        pb.matrix_basis = basis[pb.name]
        if pb.name in moving:
            continue
        for f in (0, frames):
            for path in ("location", "rotation_quaternion", "scale"):
                pb.keyframe_insert(path, frame=f)
    for name, at in moving.items():
        pb = bones[name]
        previous = None
        for f in range(frames + 1):
            loc, rot, scale = at(f).decompose()
            if previous is not None:
                rot.make_compatible(previous)
            previous = rot
            pb.location, pb.rotation_quaternion, pb.scale = loc, rot, scale
            for path in ("location", "rotation_quaternion", "scale"):
                pb.keyframe_insert(path, frame=f)


def linear(action):
    """Every key linear, so Blender plays the keys as written between frames."""
    curves = []
    for layer in getattr(action, "layers", []):
        for strip in layer.strips:
            for bag in strip.channelbags:
                curves.extend(bag.fcurves)
    if not curves and hasattr(action, "fcurves"):
        curves = list(action.fcurves)
    for fc in curves:
        for k in fc.keyframe_points:
            k.interpolation = 'LINEAR'


def new_action(rig, name):
    old = bpy.data.actions.get(name)
    if old is not None:
        bpy.data.actions.remove(old)
    action = bpy.data.actions.new(name)
    action.use_fake_user = True
    set_action(rig, action)
    return action


def make_motions(path, label):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = FPS
    scene.render.fps_base = 1
    bpy.ops.import_scene.gltf(filepath=path)
    rig = next(o for o in scene.objects if o.type == 'ARMATURE')
    if rig.animation_data is None:
        rig.animation_data_create()
    imported = (rig.animation_data.action, getattr(rig.animation_data, "action_slot", None))
    for name in ("spin", "stare"):  # a run over an earlier one's export
        if bpy.data.actions.get(name):
            bpy.data.actions.remove(bpy.data.actions[name])

    heads = stand_pose(rig)
    stand = {pb.name: pb.matrix_basis.copy() for pb in rig.pose.bones}
    sk = Skeleton(path, heads)
    print("%s (%s): %s" % (os.path.basename(path), label, sk.describe()))
    left, forward, up = body_axes(sk)
    face_below = FACE_BELOW.get(os.path.splitext(os.path.basename(path))[0], 0.0)
    body = sk.names[sk.body]
    pivot = feet_centre(rig, sk)  # the turn and the sway about the floor between the feet

    # spin: the turned pose, the body turning about the vertical through the pivot
    pose = turned_pose(rig, sk, SPIN_HEAD, face_below)
    pb = rig.pose.bones[body]
    pb.matrix_basis = pose[body]
    bpy.context.view_layer.update()
    body_matrix = pb.matrix.copy()
    to_basis = pose[body] @ body_matrix.inverted()  # armature space -> the bone's basis

    def spin_at(f):
        a = 2 * math.pi * f / SPIN_FRAMES
        sway = math.radians(SWAY_DEGREES) * math.sin(2 * a)
        r = Matrix.Rotation(a, 3, up) @ Matrix.Rotation(sway, 3, forward)
        return to_basis @ turn_about(body_matrix, pivot, r)

    for name, b in stand.items():
        rig.pose.bones[name].matrix_basis = b
    new_action(rig, "spin")
    key_bones(rig, pose, SPIN_FRAMES, {body: spin_at})
    linear(bpy.data.actions["spin"])

    # stare: the held pose
    set_action(rig, None)
    for name, b in stand.items():
        rig.pose.bones[name].matrix_basis = b
    bpy.context.view_layer.update()
    pose = turned_pose(rig, sk, STARE_HEAD, face_below)
    new_action(rig, "stare")
    key_bones(rig, pose, STARE_FRAMES)
    linear(bpy.data.actions["stare"])

    set_action(rig, imported[0])
    if imported[1] is not None:
        rig.animation_data.action_slot = imported[1]
    result = bpy.ops.export_scene.gltf(filepath=path, export_format='GLTF_SEPARATE', export_animations=True)
    if "FINISHED" not in result:
        raise RuntimeError("Blender's glTF exporter failed on " + path)
    set_steps(path, {"spin": SPIN_STEP, "stare": STARE_STEP})


def set_steps(path, steps):
    """Gives the new animations their KEYs' steps (`extras.step`, which build reads for a new KEY;
    Blender writes no extras for an action)."""
    with open(path, encoding="utf-8") as f:
        gltf = json.load(f)
    found = set()
    for animation in gltf.get("animations", []):
        if animation.get("name") in steps:
            animation.setdefault("extras", {})["step"] = steps[animation["name"]]
            found.add(animation["name"])
    if found != set(steps):
        raise RuntimeError("%s: Blender exported no animation %s" % (path, sorted(set(steps) - found)))
    with open(path, "w", encoding="utf-8") as f:
        json.dump(gltf, f, ensure_ascii=False, indent=1)


def main():
    dc2tk, iso, mods = arguments()
    exports = export_packs(dc2tk, iso, mods)
    for name, label in PACKS:
        make_motions(exports[name], label)
    print("DONE: spin and stare added to", ", ".join(n for n, _ in PACKS), "in", os.path.join(mods, MOD))


main()
