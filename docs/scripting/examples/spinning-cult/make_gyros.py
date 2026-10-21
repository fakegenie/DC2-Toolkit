# Makes the gyros, the cult's gyroscope, as one would by hand in Blender: a dark rotor with a
# blood-red spiral and a pale fish's eye on each face, spinning on an axle inside three nested
# gimbal rings of tarnished brass engraved with notches, thorns on the outer ring, on an iron stand.
# Nothing of the game's is used. Writes three things into model/ beside this script (or into the
# folder given after --):
#
#   gyros.gltf       a character pack's model: a skeleton of five bones (stand, outer, middle,
#                    inner, rotor; every pivot through the middle), each part a rigid mesh on its
#                    bone, and the action "spin" (4 s, looping): the rotor spins twelve turns, the
#                    outer ring makes one, the middle and inner rings rock on their pivots. About
#                    7 units tall and 5 across (Max is 26 tall). mod.json makes it
#                    mod:gyros.chr.
#   gyros_item.gltf  the item's model (item 429's "gyros"): the same gyroscope standing still, coarser
#                    and without bones, as the game's item models are (one mesh and the eyes', no
#                    motion: the menus that show an item's model never start a motion). mod.json
#                    makes it item/gyros.chr, like the Treasure Chest Key's takaraky.
#   gyros_icon.png   the item's menu icon, 32x32: the gyroscope drawn flat (a render of the model
#                    is mud at 32 pixels): the outer ring, the inner ring crossing in front of and
#                    behind the dark rotor with its red spiral and eye, the axle, the stand; drawn at
#                    eight times the size, shrunk, cut to opaque or clear and outlined in near-black,
#                    as the game's icons are. make_mod.py puts it in an unused cell of the disc's
#                    icon sheet in the sheet's own colours.
#
#   blender --background --factory-startup --python docs/coding/examples/spinning-cult/make_gyros.py
import bpy, bmesh, sys, math, os
import numpy as np
from mathutils import Vector, Matrix, Quaternion

here = os.path.dirname(os.path.abspath(__file__))
given = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
out_dir = (given[0] if given else os.path.join(here, "model")).replace("\\", "/")
os.makedirs(out_dir, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.fps = 30
SECONDS = 4.0
FRAMES = int(SECONDS * 30)  # keys on frames 1 to FRAMES + 1; the last is the first again
scene.frame_start = 1
scene.frame_end = FRAMES + 1

# ---------------------------------------------------------------- sizes (Blender Z up, front -Y)
R_OUT, R_MID, R_IN = 2.6, 2.05, 1.5  # the rings' middle radii
BANDS = {"outer": (0.15, 0.2), "middle": (0.13, 0.17), "inner": (0.11, 0.15)}  # radial, axial half
ROTOR_R, ROTOR_T = 1.08, 0.15  # the rotor's radius and half thickness (its axle along its X)
AXLE_R, PIN_R = 0.085, 0.075
BASE_Z = -3.45  # the stand's foot
# The gimbals: each part's pivot axis, in its parent's frame at rest. Outer turns about the
# vertical, middle about the outer's X, inner about the middle's Z, the rotor about the inner's X.
AXES = {"outer": Vector((0, 0, 1)), "middle": Vector((1, 0, 0)), "inner": Vector((0, 0, 1)), "rotor": Vector((1, 0, 0))}
PARENT = {"outer": "stand", "middle": "outer", "inner": "middle", "rotor": "inner"}
PARTS = ["stand", "outer", "middle", "inner", "rotor"]
# The pose it is shown in (degrees about each pivot): rings tilted apart so all three read.
SHOWN = {"outer": -60.0, "middle": 60.0, "inner": 30.0, "rotor": 0.0}


def shown_frames(angles):
    """Each part's rotation (3x3, about the middle) for angles about the pivots."""
    out = {"stand": Matrix.Identity(3)}
    for name in PARTS[1:]:
        out[name] = out[PARENT[name]] @ Matrix.Rotation(math.radians(angles[name]), 3, AXES[name])
    return out


# ---------------------------------------------------------------- the picture's regions (u0, v0, u1, v1)
TEX = 128
R_ENGRAVED = (0.0, 0.0, 0.5, 0.25)  # brass, notched and scratched with marks (the rings' outsides)
R_BRASS = (0.0, 0.25, 0.5, 0.5)  # brass, plain (the rings' other sides, pins)
R_DISC = (0.5, 0.0, 1.0, 0.5)  # the rotor's face: black-violet, a red spiral, a brass rim
R_RIM = (0.0, 0.5, 0.5, 0.625)  # the rotor's edge: dark steel with bright notches
R_IRON = (0.0, 0.625, 0.5, 0.75)  # the stand, the axle
R_EYE = (0.5, 0.5, 0.75, 0.75)  # a pale fish's eye with a slit pupil
R_THORN = (0.75, 0.5, 1.0, 0.75)  # dark red going black (the thorns)


def in_region(r, u, v):
    """A point (u, v in 0..1) of a region, kept a texel inside it."""
    pad = 1.0 / TEX
    return (r[0] + pad + (r[2] - r[0] - 2 * pad) * u, r[1] + pad + (r[3] - r[1] - 2 * pad) * v)


# ---------------------------------------------------------------- mesh building
class Builder:
    """A mesh in the making; points are given in a part's own frame and placed by `frame`."""

    def __init__(self, name, frame=Matrix.Identity(3)):
        self.name = name
        self.bm = bmesh.new()
        self.uv = self.bm.loops.layers.uv.new("UVMap")
        self.frame = frame

    def vert(self, p):
        return self.bm.verts.new(self.frame @ Vector(p))

    def face(self, points, uvs, hint, smooth=True):
        """A face whose normal is turned to point along `hint` (in the part's frame)."""
        verts = [self.vert(p) for p in points]
        return self.face_of(verts, uvs, self.frame @ Vector(hint), smooth)

    def face_of(self, verts, uvs, hint, smooth=True):
        pts = [v.co for v in verts]
        n = Vector((0, 0, 0))
        for i in range(len(pts)):
            a, b = pts[i], pts[(i + 1) % len(pts)]
            n += Vector(((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)))
        if n.dot(hint) < 0:
            verts, uvs = verts[::-1], uvs[::-1]
        f = self.bm.faces.new(verts)
        for loop, uv in zip(f.loops, uvs):
            loop[self.uv].uv = uv
        f.smooth = smooth
        return f

    def strip(self, rows, uvs, hints, smooth=True):
        """Quads between two rows of points (shared vertices along the strip); hints[i] the
        outward direction of quad i."""
        a = [self.vert(p) for p in rows[0]]
        b = [self.vert(p) for p in rows[1]]
        for i in range(len(a) - 1):
            self.face_of([a[i], a[i + 1], b[i + 1], b[i]], [uvs[0][i], uvs[0][i + 1], uvs[1][i + 1], uvs[1][i]],
                         self.frame @ Vector(hints[i]), smooth)

    def finish(self, collection):
        me = bpy.data.meshes.new(self.name)
        self.bm.normal_update()
        self.bm.to_mesh(me)
        self.bm.free()
        obj = bpy.data.objects.new(self.name, me)
        collection.objects.link(obj)
        return obj


def ring(b, radius, half_radial, half_axial, n, engraved=R_ENGRAVED, arcs=4):
    """A ring of rectangular section in the frame's XZ plane (its axis Y): four strips, the outside
    engraved, smooth round the ring and sharp at the corners; the picture goes round it `arcs`
    times (n a multiple of arcs)."""
    def at(theta, r, y):
        return (r * math.cos(theta), y, r * math.sin(theta))
    ro, ri = radius + half_radial, radius - half_radial
    sides = [  # (row 0, row 1, region, outward hint(theta))
        ((ro, -half_axial), (ro, half_axial), engraved, lambda t: (math.cos(t), 0, math.sin(t))),
        ((ri, half_axial), (ri, -half_axial), R_BRASS, lambda t: (-math.cos(t), 0, -math.sin(t))),
        ((ri, -half_axial), (ro, -half_axial), R_BRASS, lambda t: (0, -1, 0)),
        ((ro, half_axial), (ri, half_axial), R_BRASS, lambda t: (0, 1, 0)),
    ]
    m = n // arcs
    for k in range(arcs):
        thetas = [2 * math.pi * (k * m + i) / n for i in range(m + 1)]
        mids = [2 * math.pi * (k * m + i + 0.5) / n for i in range(m)]
        for (r0, y0), (r1, y1), region, hint in sides:
            rows = [[at(t, r0, y0) for t in thetas], [at(t, r1, y1) for t in thetas]]
            uvs = [[in_region(region, i / m, 0.0) for i in range(m + 1)], [in_region(region, i / m, 1.0) for i in range(m + 1)]]
            b.strip(rows, uvs, [hint(t) for t in mids])


def basis(d):
    d = Vector(d).normalized()
    u = d.cross(Vector((0, 0, 1)) if abs(d.z) < 0.9 else Vector((1, 0, 0))).normalized()
    return d, u, d.cross(u)


def cylinder(b, p0, p1, r, n, region, caps=False, r1=None):
    """A tube (a cone when r1 is given) from p0 to p1, u round it, v along it."""
    p0, p1 = Vector(p0), Vector(p1)
    d, u, v = basis(p1 - p0)
    r1 = r if r1 is None else r1
    thetas = [2 * math.pi * i / n for i in range(n + 1)]
    rows = [[p + rr * (math.cos(t) * u + math.sin(t) * v) for t in thetas] for p, rr in ((p0, r), (p1, r1))]
    uvs = [[in_region(region, i / n, k) for i in range(n + 1)] for k in (0.0, 1.0)]
    mids = [2 * math.pi * (i + 0.5) / n for i in range(n)]
    b.strip(rows, uvs, [tuple(math.cos(t) * u + math.sin(t) * v) for t in mids])
    if caps:
        for p, rr, sign in ((p0, r, -1), (p1, r1, 1)):
            if rr <= 0:
                continue
            pts = [p + rr * (math.cos(t) * u + math.sin(t) * v) for t in thetas[:-1]]
            b.face(pts, [in_region(region, 0.5 + 0.5 * math.cos(t), 0.5 + 0.5 * math.sin(t)) for t in thetas[:-1]],
                   tuple(sign * d), smooth=False)


def thorn(b, at, out, length, r, n=4):
    """A sharp cone from `at` along `out`."""
    out = Vector(out).normalized()
    cylinder(b, Vector(at) - out * 0.05, Vector(at) + out * length, r, n, R_THORN, r1=0.0)


def disc_face(b, x, radius, n, region, sign, centre=(0.0, 0.0), turn=0.0):
    """A flat round face at height x on the X axis (normal sign * X), mapped as a circle into
    the region; centre (y, z) on the face; turn turns the picture on it."""
    cy, cz = centre
    pts = [(x, cy + radius * math.cos(2 * math.pi * i / n), cz + radius * math.sin(2 * math.pi * i / n)) for i in range(n)]
    uvs = [in_region(region, 0.5 + 0.5 * math.cos(2 * math.pi * i / n * sign + turn), 0.5 + 0.5 * math.sin(2 * math.pi * i / n * sign + turn)) for i in range(n)]
    c = b.vert((x, cy, cz))
    rim = [b.vert(p) for p in pts]
    for i in range(n):
        j = (i + 1) % n
        b.face_of([c, rim[i], rim[j]], [in_region(region, 0.5, 0.5), uvs[i], uvs[j]], b.frame @ Vector((sign, 0, 0)), smooth=False)


def make_parts(frames, seg):
    """Every part's meshes in the shown pose, keyed by part, each named <part>_m (the bones take
    the parts' names); the eyes are a mesh of their own, its name's flag c0 drawing it unlit, so
    they glow a little in the dark."""
    collection = bpy.data.collections.new("gyros_%d" % seg)
    scene.collection.children.link(collection)
    parts = {}
    # stand: a foot, a post up to the outer ring's lower pivot, a cup it turns in
    b = Builder("stand_m", frames["stand"])
    top = -R_OUT - BANDS["outer"][0]
    cylinder(b, (0, 0, BASE_Z), (0, 0, BASE_Z + 0.28), 1.2, max(8, seg // 2), R_IRON, caps=False, r1=0.85)
    cylinder(b, (0, 0, BASE_Z + 0.28), (0, 0, BASE_Z + 0.3), 0.85, max(8, seg // 2), R_IRON, caps=True, r1=0.6)
    cylinder(b, (0, 0, BASE_Z + 0.3), (0, 0, top - 0.12), 0.13, 6, R_IRON)
    cylinder(b, (0, 0, top - 0.14), (0, 0, top + 0.02), 0.13, 6, R_BRASS, r1=0.24)
    parts["stand"] = [b]
    # outer ring: thorns round it, a spike on top, the pins the middle ring turns on (along X)
    hr, ha = BANDS["outer"]
    b = Builder("outer_m", frames["outer"])
    ring(b, R_OUT, hr, ha, seg)
    for k in range(4):
        t = math.radians(45 + 90 * k)
        thorn(b, ((R_OUT + hr) * math.cos(t), 0, (R_OUT + hr) * math.sin(t)), (math.cos(t), 0, math.sin(t)), 0.42, 0.09)
    thorn(b, (0, 0, R_OUT + hr), (0, 0, 1), 0.65, 0.11)
    for s in (1, -1):
        cylinder(b, (s * (R_MID - 0.05), 0, 0), (s * (R_OUT - hr + 0.02), 0, 0), PIN_R, 6, R_BRASS)
    parts["outer"] = [b]
    # middle ring: the pins the inner ring turns on (along Z)
    hr, ha = BANDS["middle"]
    b = Builder("middle_m", frames["middle"])
    ring(b, R_MID, hr, ha, seg)
    for s in (1, -1):
        cylinder(b, (0, 0, s * (R_IN - 0.05)), (0, 0, s * (R_MID - hr + 0.02)), PIN_R, 6, R_BRASS)
    parts["middle"] = [b]
    # inner ring: bearing caps where the axle comes through (along X)
    hr, ha = BANDS["inner"]
    b = Builder("inner_m", frames["inner"])
    ring(b, R_IN, hr, ha, seg)
    for s in (1, -1):
        cylinder(b, (s * (R_IN + hr - 0.02), 0, 0), (s * (R_IN + hr + 0.14), 0, 0), 0.15, 6, R_BRASS, caps=True, r1=0.1)
    parts["inner"] = [b]
    # rotor: a disc on its axle (along X), a brass boss either side; an eye on each face, off the
    # middle, on opposite sides, so the spin shows
    b = Builder("rotor_m", frames["rotor"])
    n = max(12, seg * 2 // 3)
    t = ROTOR_T
    disc_face(b, t, ROTOR_R, n, R_DISC, 1)
    disc_face(b, -t, ROTOR_R, n, R_DISC, -1, turn=math.pi)
    thetas = [2 * math.pi * i / n for i in range(n + 1)]
    b.strip([[(-t, ROTOR_R * math.cos(a), ROTOR_R * math.sin(a)) for a in thetas],
             [(t, ROTOR_R * math.cos(a), ROTOR_R * math.sin(a)) for a in thetas]],
            [[in_region(R_RIM, i / n, 0.0) for i in range(n + 1)], [in_region(R_RIM, i / n, 1.0) for i in range(n + 1)]],
            [(0, math.cos(a + math.pi / n), math.sin(a + math.pi / n)) for a in thetas[:-1]])
    cylinder(b, (-R_IN - 0.02, 0, 0), (R_IN + 0.02, 0, 0), AXLE_R, 6, R_IRON)
    for s in (1, -1):
        cylinder(b, (s * t, 0, 0), (s * (t + 0.16), 0, 0), 0.26, 8, R_BRASS, caps=True, r1=0.12)
    eyes = Builder("eye--c0", frames["rotor"])
    for s, (cy, cz) in ((1, (0.0, 0.6)), (-1, (0.0, -0.6))):
        disc_face(eyes, s * (t + 0.012), 0.27, 10, R_EYE, s, centre=(cy, cz), turn=math.pi / 2)
    parts["rotor"] = [b, eyes]
    return collection, {name: [x.finish(collection) for x in builders] for name, builders in parts.items()}


# ---------------------------------------------------------------- the picture
def noise(size, cells, seed):
    g = np.random.default_rng(seed).random((cells + 1, cells + 1))
    t = np.linspace(0, cells, size, endpoint=False)
    i = t.astype(int)
    f = t - i
    f = f * f * (3 - 2 * f)
    a = g[i][:, i] * (1 - f)[None, :] + g[i][:, i + 1] * f[None, :]
    b = g[i + 1][:, i] * (1 - f)[None, :] + g[i + 1][:, i + 1] * f[None, :]
    return a * (1 - f)[:, None] + b * f[:, None]


yy, xx = (np.mgrid[0:TEX, 0:TEX] + 0.5) / TEX  # yy is v (0 at the bottom), xx is u
grime = 0.6 * noise(TEX, 6, 3) + 0.4 * noise(TEX, 24, 4)
px = np.zeros((TEX, TEX, 3))


def region_mask(r):
    return (xx >= r[0]) & (xx < r[2]) & (yy >= r[1]) & (yy < r[3])


def local(r):
    """u, v across a region, 0..1."""
    return (xx - r[0]) / (r[2] - r[0]), (yy - r[1]) / (r[3] - r[1])


brass = np.array([0.62, 0.46, 0.2])
tarnish = np.array([0.24, 0.27, 0.16])  # green-black in the scratches
for region, engraved in ((R_ENGRAVED, True), (R_BRASS, False)):
    m = region_mask(region)
    u, v = local(region)
    bevel = 0.55 + 0.6 * np.sin(np.clip(v, 0, 1) * math.pi) ** 0.7  # light across the middle of a side
    col = brass[None, None, :] * bevel[..., None] * (0.85 + 0.3 * (grime - 0.5))[..., None]
    dirt = np.clip((grime - 0.45) * 2.2, 0, 1) * 0.55
    col = col * (1 - dirt[..., None]) + tarnish * dirt[..., None]
    if engraved:
        # 8 notches a quarter of a ring, every other one long, cut dark; a thin groove along it
        k = (u * 8) % 1.0
        long_ = ((np.floor(u * 8) % 2) == 0)
        notch = (np.abs(k - 0.5) < 0.1) & ((np.abs(v - 0.5) < 0.3) | long_)
        groove = np.abs(v - 0.5) < 0.05
        col = np.where((notch | groove)[..., None], np.array([0.09, 0.07, 0.04]), col)
    px[m] = col[m]
# the rotor's face: black-violet, a red double spiral winding in, a brass rim, dark at the middle
m = region_mask(R_DISC)
u, v = local(R_DISC)
qx, qy = u * 2 - 1, v * 2 - 1
rr = np.sqrt(qx ** 2 + qy ** 2)
ang = np.arctan2(qy, qx)
face = np.array([0.07, 0.035, 0.08]) * (0.8 + 0.4 * grime)[..., None]
arm = 0.5 + 0.5 * np.cos(2 * ang - rr * 11.0)
spiral = np.clip((arm - 0.72) / 0.18, 0, 1) * np.clip((rr - 0.15) / 0.15, 0, 1) * (rr < 0.86)
blood = np.array([0.55, 0.04, 0.05]) * (0.65 + 0.35 * rr)[..., None]
col = face * (1 - spiral[..., None]) + blood * spiral[..., None]
rim = np.clip(1 - np.abs(rr - 0.93) / 0.06, 0, 1)
col = col * (1 - rim[..., None]) + (brass * 0.8)[None, None, :] * rim[..., None]
px[m] = col[m]
# the rotor's edge: dark steel with twelve bright notches
m = region_mask(R_RIM)
u, v = local(R_RIM)
steel = np.array([0.16, 0.15, 0.17]) * (0.7 + 0.6 * np.sin(np.clip(v, 0, 1) * math.pi))[..., None]
mark = np.abs(((u * 12) % 1.0) - 0.5) < 0.12
px[m] = np.where(mark[..., None], brass * 1.1, steel)[m]
# iron: near-black, a little rust
m = region_mask(R_IRON)
u, v = local(R_IRON)
iron = np.array([0.11, 0.1, 0.1]) * (0.75 + 0.5 * np.sin(np.clip(u, 0, 1) * math.pi * 2) ** 2)[..., None]
rust = np.clip((grime - 0.55) * 3, 0, 1)[..., None] * 0.5
px[m] = (iron * (1 - rust) + np.array([0.25, 0.1, 0.05]) * rust)[m]
# the eye: pale sickly yellow, a black upright slit, a red rim
m = region_mask(R_EYE)
u, v = local(R_EYE)
ex, ey = u * 2 - 1, v * 2 - 1
er = np.sqrt(ex ** 2 + ey ** 2)
eye = np.array([0.95, 0.9, 0.55]) * (1.0 - 0.35 * er ** 2)[..., None]
slit = (np.abs(ex) < 0.16 * np.sqrt(np.clip(1 - ey ** 2, 0, 1))) & (er < 0.8)
eye = np.where(slit[..., None], np.array([0.02, 0.0, 0.01]), eye)
eye = np.where((er > 0.82)[..., None], np.array([0.5, 0.05, 0.04]), eye)
px[m] = eye[m]
# thorns: dark red at the root, black at the tip (v along the thorn)
m = region_mask(R_THORN)
u, v = local(R_THORN)
px[m] = (np.array([0.32, 0.03, 0.03]) * (1 - np.clip(v, 0, 1))[..., None] + 0.02)[m]

img = bpy.data.images.new("gyros_skin", TEX, TEX)
rgba = np.concatenate([np.clip(px, 0, 1), np.ones((TEX, TEX, 1))], axis=2)
img.pixels.foreach_set(rgba.astype(np.float32).ravel())
img.filepath_raw = out_dir + "/gyros_skin.png"
img.file_format = 'PNG'
img.save()
mat = bpy.data.materials.new("gyros_skin")
tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
tex.image = img
tex.interpolation = 'Closest'
bsdf = mat.node_tree.nodes["Principled BSDF"]
mat.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
bsdf.inputs["Roughness"].default_value = 0.55
bsdf.inputs["Metallic"].default_value = 0.0


def discard(collection):
    """Removes a collection's objects and meshes, so the next build's names are free."""
    for ob in list(collection.objects):
        data = ob.data
        bpy.data.objects.remove(ob)
        if isinstance(data, bpy.types.Mesh):
            bpy.data.meshes.remove(data)
    bpy.data.collections.remove(collection)


def textured(objects):
    for ob in objects:
        ob.data.materials.append(mat)


def export(path, objects, animations):
    bpy.ops.object.select_all(action='DESELECT')
    for ob in objects:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.export_scene.gltf(filepath=path, export_format='GLTF_SEPARATE', use_selection=True,
                              export_animations=animations, export_image_format='AUTO')
    print("WROTE", path)


rest = shown_frames(SHOWN)

# ---------------------------------------------------------------- the character: bones and "spin"
coll, parts = make_parts(rest, 28)
arm_data = bpy.data.armatures.new("gyros_rig")
rig = bpy.data.objects.new("gyros", arm_data)
coll.objects.link(rig)
bpy.context.view_layer.objects.active = rig
bpy.ops.object.mode_set(mode='EDIT')
for name in PARTS:
    eb = arm_data.edit_bones.new(name)
    eb.head, eb.tail = Vector((0, 0, 0)), Vector((0, 0, 1))
    m = rest[name].to_4x4()
    if name == "stand":
        m.translation = Vector((0, 0, BASE_Z))
    eb.length = 0.8
    eb.matrix = m
    if name != "stand":
        eb.parent = arm_data.edit_bones[PARENT[name]]
        eb.use_connect = False
bpy.ops.object.mode_set(mode='OBJECT')
meshes = []
for name in PARTS:
    for ob in parts[name]:
        ob.parent = rig
        ob.parent_type = 'BONE'
        ob.parent_bone = name
        bpy.context.view_layer.update()
        ob.matrix_world = Matrix.Identity(4)
        meshes.append(ob)
textured(meshes)

P = rig.pose.bones
for pb in P:
    pb.rotation_mode = 'QUATERNION'


def spin_angles(t):
    """Degrees about each pivot from the shown pose, t in 0..1 of the loop."""
    w = 2 * math.pi * t
    return {
        "outer": 360.0 * t,  # one slow turn
        "middle": 22.0 * math.sin(w),  # rocking on the outer ring's pins
        "inner": 30.0 * math.sin(2 * w + 0.7),  # twice as fast on the middle ring's pins
        "rotor": 360.0 * 12 * t,  # twelve turns: 36 degrees a frame
    }


def pose_quaternion(name, degrees):
    """A bone's rotation for a turn about its pivot: the pivot axis in the bone's own axes."""
    axis_rest = rest[PARENT[name]] @ AXES[name]
    local_axis = (arm_data.bones[name].matrix_local.to_3x3().inverted() @ axis_rest).normalized()
    return Quaternion(local_axis, math.radians(degrees))


def set_keys(action, path, index, values):
    fc = action.fcurve_ensure_for_datablock(rig, path, index=index)
    fc.keyframe_points.add(len(values))
    fc.keyframe_points.foreach_set("co", [x for f, v in enumerate(values) for x in (1 + f, v)])
    for k in fc.keyframe_points:
        k.interpolation = 'LINEAR'
    fc.update()


rig.animation_data_create()
action = bpy.data.actions.new("spin")
rig.animation_data.action = action
for name in PARTS[1:]:
    qs = []
    for f in range(FRAMES + 1):
        q = pose_quaternion(name, spin_angles(f / FRAMES)[name])
        if qs and q.dot(qs[-1]) < 0:
            q.negate()
        qs.append(q)
    for i in range(4):
        set_keys(action, f'pose.bones["{name}"].rotation_quaternion', i, [q[i] for q in qs])
action.use_fake_user = True
scene.frame_set(1)
export(out_dir + "/gyros.gltf", [rig] + meshes, True)
discard(coll)

# ---------------------------------------------------------------- the item: one still mesh, coarser
coll_item, item_parts = make_parts(rest, 20)
body = [ob for name in PARTS for ob in item_parts[name] if not ob.name.startswith("eye")]
eyes = [ob for ob in item_parts["rotor"] if ob.name.startswith("eye")][0]
bpy.ops.object.select_all(action='DESELECT')
for ob in body:
    ob.select_set(True)
bpy.context.view_layer.objects.active = body[0]
bpy.ops.object.join()
item = bpy.context.view_layer.objects.active
item.name = item.data.name = "gyros"
textured([item, eyes])
export(out_dir + "/gyros_item.gltf", [item, eyes], False)
discard(coll_item)


# ---------------------------------------------------------------- the icon: drawn, 32x32
def draw_icon(size=32, ss=8):
    """The gyros as a 32x32 menu icon: rows from the top, RGBA 0..1, alpha 0 or 1, outlined."""
    n = size * ss
    ys, xs = (np.mgrid[0:n, 0:n] + 0.5) / ss  # pixel units, y down
    col = np.zeros((n, n, 3))
    cover = np.zeros((n, n), dtype=bool)
    ink = np.array([0.07, 0.04, 0.05])
    light = np.array([-0.6, -0.8])  # from the top left

    def frame(cx, cy, angle):
        c, s = math.cos(math.radians(angle)), math.sin(math.radians(angle))
        x, y = xs - cx, ys - cy
        return c * x + s * y, -s * x + c * y

    def ellipse_dist(u, v, rx, ry):
        f = (u / rx) ** 2 + (v / ry) ** 2 - 1
        g = np.sqrt((2 * u / rx ** 2) ** 2 + (2 * v / ry ** 2) ** 2) + 1e-9
        return f / g

    def put(mask, colour):
        col[mask] = colour[mask] if colour.ndim == 3 else colour
        cover[mask] = True

    def brass(shade):
        lo, mid, hi = np.array([0.36, 0.22, 0.07]), np.array([0.8, 0.58, 0.2]), np.array([1.0, 0.9, 0.58])
        s = np.clip(shade, 0, 1)[..., None]
        return np.where(s < 0.6, lo + (mid - lo) * (s / 0.6), mid + (hi - mid) * ((s - 0.6) / 0.4))

    def ring(cx, cy, rx, ry, angle, half, keep=None, notches=0, base=0.0):
        u, v = frame(cx, cy, angle)
        d = ellipse_dist(u, v, rx, ry)
        band = np.abs(d) < half
        if keep is not None:
            band &= keep(u, v)
        phi = np.arctan2(v / ry, u / rx)
        # light: the normal of the curve in the picture, against the light; brighter mid-band
        a = math.radians(angle)
        nx, ny = np.cos(phi) * math.cos(a) - np.sin(phi) * math.sin(a), np.cos(phi) * math.sin(a) + np.sin(phi) * math.cos(a)
        lit = -(nx * light[0] + ny * light[1])
        across = 1 - (d / half) ** 2
        shade = 0.42 + 0.33 * lit + 0.3 * across + base
        c = brass(shade)
        if notches:
            tick = np.abs(((phi / (2 * math.pi)) * notches) % 1.0 - 0.5) < 0.07
            c = np.where((tick & (np.abs(d) < half * 0.55))[..., None], np.array([0.2, 0.12, 0.04]), c)
        edge = band & (np.abs(d) > half - 0.7)
        c = np.where(edge[..., None], ink, c)
        put(band, c)

    def disc(cx, cy, rx, ry, angle):
        u, v = frame(cx, cy, angle)
        d = ellipse_dist(u, v, rx, ry)
        inside = d < 0
        r = np.sqrt((u / rx) ** 2 + (v / ry) ** 2)
        th = np.arctan2(v / ry, u / rx)
        face = np.array([0.12, 0.05, 0.13]) * (1.15 - 0.5 * r)[..., None]
        arm = 0.5 + 0.5 * np.cos(2 * th - r * 7.5)
        spiral = (arm > 0.62) & (r > 0.18) & (r < 0.82)
        c = np.where(spiral[..., None], np.array([0.72, 0.06, 0.07]), face)
        eye = ((u - 0.15 * rx) ** 2 + (v + 0.45 * ry) ** 2) < (0.2 * min(rx, ry)) ** 2
        c = np.where(eye[..., None], np.array([0.95, 0.9, 0.55]), c)
        pupil = eye & (np.abs(u - 0.15 * rx) < 0.25)
        c = np.where(pupil[..., None], ink, c)
        rim = inside & (d > -1.0)
        c = np.where(rim[..., None], brass(0.55 + 0.3 * -(np.cos(th) * light[0] + np.sin(th) * light[1]))[...], c)
        c = np.where((inside & (d > -0.6))[..., None], ink, c)
        put(inside, c)

    def bar(x0, y0, x1, y1, half, colour):
        px, py = xs - x0, ys - y0
        dx, dy = x1 - x0, y1 - y0
        t = np.clip((px * dx + py * dy) / (dx * dx + dy * dy), 0, 1)
        dist = np.sqrt((px - t * dx) ** 2 + (py - t * dy) ** 2)
        m = dist < half
        c = np.where((dist > half - 0.7)[..., None], ink, np.broadcast_to(colour, (n, n, 3)))
        put(m, c)

    cx, cy = 16.0, 13.6
    # the stand: a foot and a post
    u, v = frame(16, 29.2, 0)
    foot = ((u / 7.2) ** 2 + (v / 2.2) ** 2) < 1
    fshade = np.array([0.2, 0.19, 0.22]) * (1.25 - 0.08 * (xs - 10))[..., None]
    fshade = np.where((((u / 7.2) ** 2 + (v / 2.2) ** 2) > 0.62)[..., None], ink, fshade)
    put(foot, np.clip(fshade, 0, 1))
    bar(16, 24.5, 16, 28.5, 1.25, np.array([0.25, 0.23, 0.26]))
    # the outer ring (a thorn on top), the inner ring's back half, the axle, the rotor, the front half
    bar(16, -0.2, 16, 2.6, 0.9, np.array([0.55, 0.05, 0.05]))
    ring(cx, cy, 11.4, 12.2, -8, 2.0, notches=12)
    bar(16 - 11.2, cy + 0.6, 16 - 8.4, cy + 0.3, 0.9, np.array([0.8, 0.6, 0.22]))  # the pins
    bar(16 + 11.2, cy - 0.6, 16 + 8.4, cy - 0.3, 0.9, np.array([0.8, 0.6, 0.22]))
    ring(cx, cy, 8.4, 3.5, -24, 1.6, keep=lambda u, v: v < 0, base=-0.05)
    bar(cx - 6.6, cy + 4.2, cx + 6.6, cy - 4.2, 0.85, np.array([0.3, 0.28, 0.32]))
    disc(cx, cy, 4.4, 6.2, -32)
    ring(cx, cy, 8.4, 3.5, -24, 1.6, keep=lambda u, v: v >= 0, base=-0.05)
    # down to 32: a pixel is solid where the shapes cover half of it; then the outline
    a = cover.reshape(size, ss, size, ss).mean(axis=(1, 3))
    c = (col * cover[..., None]).reshape(size, ss, size, ss, 3).sum(axis=(1, 3)) / np.maximum(cover.reshape(size, ss, size, ss).sum(axis=(1, 3)), 1)[..., None]
    solid = a >= 0.5
    grown = solid.copy()
    grown[1:, :] |= solid[:-1, :]
    grown[:-1, :] |= solid[1:, :]
    grown[:, 1:] |= solid[:, :-1]
    grown[:, :-1] |= solid[:, 1:]
    icon = np.zeros((size, size, 4))
    icon[grown & ~solid] = (*ink, 1.0)
    icon[solid, :3] = c[solid]
    icon[solid, 3] = 1.0
    return icon


icon = draw_icon()
out_icon = bpy.data.images.new("gyros_icon", 32, 32, alpha=True)
out_icon.pixels.foreach_set(icon[::-1].astype(np.float32).ravel())  # Blender's rows go up
out_icon.filepath_raw = out_dir + "/gyros_icon.png"
out_icon.file_format = 'PNG'
out_icon.save()
print("WROTE", out_dir + "/gyros_icon.png")
