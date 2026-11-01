#!/usr/bin/env python3
"""Author the Berdly-inspired first-person arm and bake its feather motion.

Everything here is original geometry, an original rig and original materials. Nothing is
extracted from HYPER DEMON or DELTARUNE. The arm is designed for the pipeline measured in
``docs/RE_HAND.md``: <=16 skinning bones per vertex pair, position/normal/tangent/uv0/uv1
plus uv2/uv3 for indices and weights, and a rigid first-person root that the game can drive.

Usage:
    blender -b --python tools/assets/build_berdly_arm.py
    blender -b --python tools/assets/build_berdly_arm.py -- --no-render

Outputs under ``assets/berdly-arm``:
    berdly_arm.blend          editable project (mesh, armature, weights, actions)
    berdly_arm.glb            viewable export
    berdly_arm.mesh.json      runtime description (mesh + bones + spring parameters)
    renders/*.png             first-person, side, three-quarter and feather close-ups
"""

from __future__ import annotations

import json
import math
import os
import random
import sys
from pathlib import Path

import bpy
from mathutils import Euler, Vector

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "assets" / "berdly-arm"
RENDERS = OUT / "renders"
RNG = random.Random(24731)

# --------------------------------------------------------------------------- palette

BLUE = (0.003, 0.035, 0.420, 1.0)
BLUE_LIGHT = (0.010, 0.160, 0.850, 1.0)
CYAN = (0.02, 0.55, 1.00, 1.0)
CREAM = (0.38, 0.30, 0.18, 1.0)
CLAW = (0.030, 0.022, 0.018, 1.0)

MAT_BLUE = 0
MAT_LIGHT = 1
MAT_CREAM = 2
MAT_CLAW = 3
MAT_IRID = 4

# --------------------------------------------------------------------------- helpers


def vec(x, y=None, z=None):
    if y is None:
        return Vector(x)
    return Vector((x, y, z))


def smooth(a: float, b: float, t: float) -> float:
    if a == b:
        return 1.0 if t >= b else 0.0
    u = max(0.0, min(1.0, (t - a) / (b - a)))
    return u * u * (3.0 - 2.0 * u)


def lerp(a, b, t):
    return a + (b - a) * t


def safe_normal(v: Vector, fallback=Vector((0.0, 0.0, 1.0))) -> Vector:
    if v.length < 1e-8:
        return fallback.copy()
    return v.normalized()


def frame_for(direction: Vector, up_hint=Vector((0.0, 0.0, 1.0))):
    forward = safe_normal(direction)
    up = up_hint - forward * up_hint.dot(forward)
    if up.length < 1e-6:
        up = Vector((0.0, 1.0, 0.0)) - forward * forward.y
    up.normalize()
    right = forward.cross(up).normalized()
    return right, up, forward


def add_material(name: str, base, *, metallic=0.35, roughness=0.30,
                 iridescence=0.55, emission=0.22, ramp=("blue", "cyan")):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    mat.use_backface_culling = False
    mat.diffuse_color = base
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    nodes.clear()

    out = nodes.new("ShaderNodeOutputMaterial")
    out.location = (620, 0)
    bsdf = nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.location = (300, 0)
    links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])

    def set_input(node, name, value):
        socket = node.inputs.get(name)
        if socket is not None:
            socket.default_value = value

    set_input(bsdf, "Base Color", base)
    set_input(bsdf, "Metallic", metallic)
    set_input(bsdf, "Roughness", roughness)
    set_input(bsdf, "IOR", 1.45)
    set_input(bsdf, "Coat Weight", 0.30)
    set_input(bsdf, "Coat Roughness", 0.24)
    set_input(bsdf, "Sheen Weight", 0.18)
    set_input(bsdf, "Sheen Roughness", 0.40)
    set_input(bsdf, "Sheen Tint", (0.35, 0.65, 1.0, 1.0))
    set_input(bsdf, "Emission Strength", emission)

    # View-dependent prismatic rim. This is the Blender-side stand-in for the game's
    # reflection/refract/edgespec path; the runtime keeps the game's entity_hand material.
    texco = nodes.new("ShaderNodeTexCoord")
    texco.location = (-900, 120)
    layer = nodes.new("ShaderNodeLayerWeight")
    layer.location = (-700, 220)
    set_input(layer, "Blend", 0.18)
    links.new(texco.outputs["Generated"], layer.inputs["Normal"])

    ramp_node = nodes.new("ShaderNodeValToRGB")
    ramp_node.location = (-500, 240)
    ramp_node.color_ramp.interpolation = "EASE"
    elements = ramp_node.color_ramp.elements
    elements[0].position = 0.0
    elements[0].color = (0.02, 0.12, 0.85, 1.0)
    elements[1].position = 1.0
    elements[1].color = (0.02, 0.55, 1.0, 1.0)
    if ramp == "prism":
        palette = [
            (0.01, 0.05, 0.55, 1.0),   # deep blue
            (0.03, 0.32, 0.95, 1.0),   # blue
            (0.02, 0.62, 1.00, 1.0),   # azure
            (0.34, 0.12, 0.90, 1.0),   # violet highlight
            (0.02, 0.55, 1.00, 1.0),   # azure again
        ]
        for index, colour in enumerate(palette):
            if index < 2:
                elements[index].position = index / (len(palette) - 1)
                elements[index].color = colour
            else:
                element = elements.new(index / (len(palette) - 1))
                element.color = colour
    elif ramp == "cream":
        elements[0].position = 0.0
        elements[0].color = (0.55, 0.48, 0.35, 1.0)
        elements[1].position = 1.0
        elements[1].color = (1.0, 0.95, 0.82, 1.0)
    links.new(layer.outputs["Facing"], ramp_node.inputs["Fac"])

    mix = nodes.new("ShaderNodeMix")
    mix.data_type = "RGBA"
    mix.location = (40, 80)
    set_input(mix, "Factor", iridescence)
    set_input(mix, "A", base)
    mix.inputs[6].default_value = base
    links.new(ramp_node.outputs["Color"], mix.inputs[7])

    # Mix's RGBA sockets are not stable by name in every build; index 6/7 are A/B.
    links.new(mix.outputs[2], bsdf.inputs["Base Color"])
    links.new(ramp_node.outputs["Color"], bsdf.inputs["Emission Color"])

    # Fine barbs / scale striations.
    noise = nodes.new("ShaderNodeTexNoise")
    noise.location = (-700, -260)
    set_input(noise, "Scale", 42.0)
    set_input(noise, "Detail", 5.0)
    set_input(noise, "Roughness", 0.65)
    links.new(texco.outputs["Generated"], noise.inputs["Vector"])
    bump = nodes.new("ShaderNodeBump")
    bump.location = (40, -260)
    set_input(bump, "Strength", 0.22)
    set_input(bump, "Distance", 0.008)
    links.new(noise.outputs["Fac"], bump.inputs["Height"])
    links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])

    return mat


# --------------------------------------------------------------------------- mesh builder


class MeshBuilder:
    def __init__(self):
        self.verts: list[Vector] = []
        self.faces: list[tuple[int, ...]] = []
        self.face_mats: list[int] = []
        self.loop_uvs: list[tuple[float, float]] = []
        self.weights: list[dict[str, float]] = []

    def vertex(self, co, weights):
        self.verts.append(Vector(co))
        normalised = {name: max(0.0, weight) for name, weight in weights.items()
                      if weight > 1e-4}
        total = sum(normalised.values()) or 1.0
        self.weights.append({name: weight / total for name, weight in normalised.items()})
        return len(self.verts) - 1

    def face(self, indices, material, uvs):
        self.faces.append(tuple(indices))
        self.face_mats.append(material)
        self.loop_uvs.extend(uvs)

    def quad(self, a, b, c, d, material, uvs):
        self.face((a, b, c, d), material, uvs)

    def tri(self, a, b, c, material, uvs):
        self.face((a, b, c), material, uvs)

    def to_object(self, name, materials):
        mesh = bpy.data.meshes.new(name)
        mesh.from_pydata([tuple(v) for v in self.verts], [], [list(f) for f in self.faces])
        mesh.update()
        for material in materials:
            mesh.materials.append(material)
        for index, polygon in enumerate(mesh.polygons):
            polygon.material_index = self.face_mats[index]
            polygon.use_smooth = False
        uv_layer = mesh.uv_layers.new(name="UVMap")
        for index, loop in enumerate(mesh.loops):
            if index < len(self.loop_uvs):
                uv_layer.data[index].uv = self.loop_uvs[index]
        mesh.validate(verbose=False)
        mesh.update()
        obj = bpy.data.objects.new(name, mesh)
        bpy.context.scene.collection.objects.link(obj)
        for vertex_index, group_weights in enumerate(self.weights):
            for group_name, weight in group_weights.items():
                group = obj.vertex_groups.get(group_name)
                if group is None:
                    group = obj.vertex_groups.new(name=group_name)
                group.add([vertex_index], weight, "REPLACE")
        return obj


def tube(builder: MeshBuilder, path, radii, sides, material, weight_for,
         uv_scale=1.0, cap=True, mat_tip=None):
    rings = []
    path = [Vector(p) for p in path]
    for index, point in enumerate(path):
        if index == 0:
            tangent = path[1] - path[0]
        elif index == len(path) - 1:
            tangent = path[-1] - path[-2]
        else:
            tangent = path[index + 1] - path[index - 1]
        right, up, forward = frame_for(tangent)
        ring = []
        for side in range(sides):
            angle = 2.0 * math.pi * side / sides
            offset = right * (math.cos(angle) * radii[index]) + up * (math.sin(angle) * radii[index])
            weights = weight_for(index / max(1, len(path) - 1), point + offset)
            ring.append(builder.vertex(point + offset, weights))
        rings.append(ring)
    for index in range(len(path) - 1):
        for side in range(sides):
            a = rings[index][side]
            b = rings[index][(side + 1) % sides]
            c = rings[index + 1][(side + 1) % sides]
            d = rings[index + 1][side]
            u0 = side / sides
            u1 = (side + 1) / sides
            v0 = index / (len(path) - 1) * uv_scale
            v1 = (index + 1) / (len(path) - 1) * uv_scale
            mat = material
            if mat_tip is not None and index >= len(path) - 2:
                mat = mat_tip
            builder.quad(a, b, c, d, mat,
                         [(u0, v0), (u1, v0), (u1, v1), (u0, v1)])
    if cap:
        start_center = builder.vertex(path[0], weight_for(0.0, path[0]))
        end_center = builder.vertex(path[-1], weight_for(1.0, path[-1]))
        for side in range(sides):
            a = rings[0][side]
            b = rings[0][(side + 1) % sides]
            builder.tri(start_center, b, a, material,
                        [(0.5, 0.0), ((side + 1) / sides, 0.0), (side / sides, 0.0)])
            c = rings[-1][side]
            d = rings[-1][(side + 1) % sides]
            builder.tri(end_center, c, d, mat_tip if mat_tip is not None else material,
                        [(0.5, 1.0), (side / sides, 1.0), ((side + 1) / sides, 1.0)])
    return rings


def ellipsoid(builder: MeshBuilder, center, scale, material, weights,
              rings=8, segments=16):
    center = Vector(center)
    top = builder.vertex(center + Vector((0.0, 0.0, scale[2])), weights)
    bottom = builder.vertex(center - Vector((0.0, 0.0, scale[2])), weights)
    grid = []
    for ring in range(1, rings):
        phi = math.pi * ring / rings
        row = []
        for segment in range(segments):
            theta = 2.0 * math.pi * segment / segments
            point = center + Vector((
                math.sin(phi) * math.cos(theta) * scale[0],
                math.sin(phi) * math.sin(theta) * scale[1],
                math.cos(phi) * scale[2],
            ))
            row.append(builder.vertex(point, weights))
        grid.append(row)
    for segment in range(segments):
        builder.tri(top, grid[0][segment], grid[0][(segment + 1) % segments], material,
                    [(0.5, 1.0), (segment / segments, 0.9),
                     ((segment + 1) / segments, 0.9)])
        builder.tri(bottom, grid[-1][(segment + 1) % segments], grid[-1][segment], material,
                    [(0.5, 0.0), ((segment + 1) / segments, 0.1), (segment / segments, 0.1)])
    for ring in range(len(grid) - 1):
        for segment in range(segments):
            a = grid[ring][segment]
            b = grid[ring][(segment + 1) % segments]
            c = grid[ring + 1][(segment + 1) % segments]
            d = grid[ring + 1][segment]
            builder.quad(a, b, c, d, material,
                         [(segment / segments, ring / rings),
                          ((segment + 1) / segments, ring / rings),
                          ((segment + 1) / segments, (ring + 1) / rings),
                          (segment / segments, (ring + 1) / rings)])


def octahedron(builder: MeshBuilder, center, size, material, weights):
    """A small faceted gem: eight triangular faces, no smooth normals."""
    center = Vector(center)
    sx, sy, sz = size * 0.78, size, size * 0.62
    points = [center + Vector((sx, 0.0, 0.0)),
              center + Vector((-sx, 0.0, 0.0)),
              center + Vector((0.0, sy, 0.0)),
              center + Vector((0.0, -sy, 0.0)),
              center + Vector((0.0, 0.0, sz)),
              center + Vector((0.0, 0.0, -sz))]
    indices = [builder.vertex(point, weights) for point in points]
    top, bottom = indices[4], indices[5]
    ring = indices[0:4]
    for index in range(4):
        a = ring[index]
        b = ring[(index + 1) % 4]
        builder.tri(top, a, b, material,
                    [(0.5, 1.0), (index / 4.0, 0.65), ((index + 1) / 4.0, 0.65)])
        builder.tri(bottom, b, a, material,
                    [(0.5, 0.0), ((index + 1) / 4.0, 0.35), (index / 4.0, 0.35)])


def feather(builder: MeshBuilder, base, direction, length, width, curve, bone,
            material, tip_material, side_hint=Vector((0.0, 0.0, 1.0)),
            thickness=0.0026, parent_bone=None, twist=0.0):
    """One layered feather card with a raised rachis and a closed thin edge."""
    base = Vector(base)
    forward, up, right = frame_for(Vector(direction), side_hint)
    if abs(twist) > 1e-6:
        rotation = Euler((0.0, twist, 0.0), "XYZ").to_matrix()
        forward = rotation @ forward
        up = rotation @ up
        right = rotation @ right

    rows = 8
    cols = 3
    top = []
    bottom = []
    for row in range(rows + 1):
        t = row / rows
        taper = smooth(0.0, 0.16, t) * math.sin(math.pi * min(1.0, t * 0.92 + 0.04)) ** 0.55
        if t > 0.78:
            taper *= max(0.02, (1.0 - t) / 0.22) ** 0.80
        taper *= 1.0 + 0.05 * math.sin(t * math.pi * 5.0)
        half = max(0.0015, width * 0.5 * taper)
        centre = (base + forward * (length * t)
                  + up * (curve * t * t)
                  + right * (math.sin(t * math.pi) * 0.012))
        top_row = []
        bottom_row = []
        for col in range(cols):
            across = (col - 1) * half
            ridge = 0.0035 * (1.0 - abs(col - 1)) * taper
            weight = 1.0 if parent_bone is None else lerp(0.72, 0.25, t)
            weights = {bone: weight}
            if parent_bone is not None:
                weights[parent_bone] = 1.0 - weight
            for layer, target in ((-1.0, bottom_row), (1.0, top_row)):
                point = (centre + right * across
                         + up * (ridge + layer * thickness * (0.45 + 0.55 * (1.0 - t))))
                target.append(builder.vertex(point, weights))
        top.append(top_row)
        bottom.append(bottom_row)

    for row in range(rows):
        for col in range(cols - 1):
            u0 = col / (cols - 1)
            u1 = (col + 1) / (cols - 1)
            v0 = row / rows
            v1 = (row + 1) / rows
            mat = tip_material if row >= int(rows * 0.68) else material
            builder.quad(top[row][col], top[row][col + 1],
                         top[row + 1][col + 1], top[row + 1][col],
                         mat, [(u0, v0), (u1, v0), (u1, v1), (u0, v1)])
            builder.quad(bottom[row][col + 1], bottom[row][col],
                         bottom[row + 1][col], bottom[row + 1][col + 1],
                         mat, [(u1, v0), (u0, v0), (u0, v1), (u1, v1)])
    # Close the long edges and the tip so the card reads as a solid blade.
    for row in range(rows):
        for col in (0, cols - 1):
            a = top[row][col]
            b = top[row + 1][col]
            c = bottom[row + 1][col]
            d = bottom[row][col]
            builder.quad(a, b, c, d, material,
                         [(0.0, row / rows), (0.0, (row + 1) / rows),
                          (1.0, (row + 1) / rows), (1.0, row / rows)])
    builder.quad(top[rows][0], bottom[rows][0], bottom[rows][1], top[rows][1],
                 tip_material, [(0.0, 1.0), (0.0, 1.0), (1.0, 1.0), (1.0, 1.0)])
    builder.quad(top[rows][1], bottom[rows][1], bottom[rows][2], top[rows][2],
                 tip_material, [(0.0, 1.0), (0.0, 1.0), (1.0, 1.0), (1.0, 1.0)])

    # A raised, contrasting rachis makes the card read as a feather rather than a leaf.
    def shaft_weights(t, _point):
        weight = 1.0 if parent_bone is None else lerp(0.72, 0.25, t)
        result = {bone: weight}
        if parent_bone is not None:
            result[parent_bone] = 1.0 - weight
        return result

    shaft = [base + forward * (length * t) + up * (curve * t * t + 0.0025)
             for t in (0.03, 0.34, 0.68, 0.97)]
    tube(builder, shaft, [0.0028, 0.0020, 0.0012, 0.0004], 4, MAT_LIGHT,
         shaft_weights, uv_scale=1.0, cap=False, mat_tip=MAT_LIGHT)


# --------------------------------------------------------------------------- rig


BONES = [
    ("root", (0.10, -0.38, 0.01), (0.10, -0.31, 0.03), None),
    ("forearm_01", (0.10, -0.31, 0.03), (0.06, -0.08, 0.07), "root"),
    ("forearm_02", (0.06, -0.08, 0.07), (0.01, 0.20, 0.11), "forearm_01"),
    ("wrist", (0.01, 0.20, 0.11), (0.00, 0.34, 0.10), "forearm_02"),
    ("hand", (0.00, 0.34, 0.10), (0.00, 0.47, 0.095), "wrist"),
    ("finger_a_01", (0.050, 0.440, 0.100), (0.082, 0.490, 0.165), "hand"),
    ("finger_a_02", (0.082, 0.490, 0.165), (0.060, 0.560, 0.270), "finger_a_01"),
    ("finger_b_01", (0.000, 0.450, 0.105), (0.000, 0.500, 0.180), "hand"),
    ("finger_b_02", (0.000, 0.500, 0.180), (0.000, 0.560, 0.300), "finger_b_01"),
    ("finger_c_01", (-0.050, 0.440, 0.100), (-0.082, 0.490, 0.165), "hand"),
    ("finger_c_02", (-0.082, 0.490, 0.165), (-0.060, 0.560, 0.270), "finger_c_01"),
    ("thumb_01", (-0.050, 0.390, 0.090), (-0.100, 0.410, 0.140), "hand"),
    ("thumb_02", (-0.100, 0.410, 0.140), (-0.020, 0.470, 0.215), "thumb_01"),
    ("feather_up_01", (0.085, -0.195, 0.108), (0.100, -0.340, 0.200), "forearm_01"),
    ("feather_up_02", (0.057, -0.030, 0.125), (0.070, -0.170, 0.220), "forearm_02"),
    ("feather_up_03", (0.033, 0.110, 0.142), (0.045, -0.040, 0.240), "wrist"),
    ("feather_outer_01", (0.140, -0.170, 0.050), (0.280, -0.300, 0.010), "forearm_01"),
    ("feather_outer_02", (0.095, 0.000, 0.080), (0.240, -0.120, 0.040), "forearm_02"),
    ("feather_inner_01", (0.020, -0.090, 0.065), (-0.100, -0.230, 0.010), "forearm_01"),
    ("feather_inner_02", (0.000, 0.040, 0.087), (-0.120, -0.100, 0.030), "forearm_02"),
    ("feather_primary", (0.040, 0.220, 0.160), (-0.060, -0.020, 0.280), "wrist"),
    ("feather_alula", (0.020, 0.160, 0.150), (0.060, 0.020, 0.240), "wrist"),
    ("claw_a", (0.060, 0.560, 0.270), (0.030, 0.550, 0.315), "finger_a_02"),
    ("claw_b", (0.000, 0.560, 0.300), (0.000, 0.550, 0.350), "finger_b_02"),
    ("claw_c", (-0.060, 0.560, 0.270), (-0.030, 0.550, 0.315), "finger_c_02"),
    ("claw_t", (-0.020, 0.470, 0.215), (0.020, 0.480, 0.250), "thumb_02"),
]

FEATHER_BONES = [name for name, *_ in BONES if name.startswith("feather_")]


def create_rig():
    arm_data = bpy.data.armatures.new("BerdlyArmRig")
    arm_obj = bpy.data.objects.new("BerdlyArmRig", arm_data)
    bpy.context.scene.collection.objects.link(arm_obj)
    bpy.context.view_layer.objects.active = arm_obj
    arm_obj.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    edit_bones = {}
    for name, head, tail, parent in BONES:
        bone = arm_data.edit_bones.new(name)
        bone.head = head
        bone.tail = tail
        bone.use_deform = True
        if parent:
            bone.parent = edit_bones[parent]
            bone.use_connect = False
        edit_bones[name] = bone
    bpy.ops.object.mode_set(mode="OBJECT")
    for pose_bone in arm_obj.pose.bones:
        pose_bone.rotation_mode = "XYZ"
    return arm_obj


# --------------------------------------------------------------------------- geometry

FOREARM_PATH = [
    (0.108, -0.340, 0.020), (0.100, -0.285, 0.032), (0.090, -0.230, 0.043),
    (0.080, -0.170, 0.052), (0.071, -0.110, 0.062), (0.062, -0.045, 0.074),
    (0.052, 0.022, 0.085), (0.040, 0.092, 0.096), (0.028, 0.160, 0.105),
    (0.016, 0.225, 0.110), (0.006, 0.290, 0.108), (0.002, 0.335, 0.103),
]

FOREARM_RADII = [0.058, 0.064, 0.061, 0.056, 0.051, 0.047, 0.044, 0.042,
                 0.043, 0.048, 0.053, 0.049]


def forearm_weights(t, _point):
    if t < 0.30:
        w = smooth(0.0, 0.30, t)
        return {"forearm_01": 1.0 - 0.55 * w, "forearm_02": 0.55 * w}
    if t < 0.62:
        w = smooth(0.30, 0.62, t)
        return {"forearm_01": 0.45 * (1.0 - w), "forearm_02": 0.55 + 0.30 * w,
                "wrist": 0.15 * w}
    w = smooth(0.62, 0.92, t)
    return {"forearm_02": 0.30 * (1.0 - w), "wrist": 0.55 + 0.15 * w,
            "hand": 0.15 * w}


def finger_weights(bone_a, bone_b, t):
    if t < 0.55:
        return {bone_a: 1.0}
    w = smooth(0.55, 0.95, t)
    return {bone_a: 1.0 - w, bone_b: w}


def forearm_sample(t):
    """Interpolate the forearm centreline and its radius, with a stable frame."""
    t = max(0.0, min(1.0, t))
    position = t * (len(FOREARM_PATH) - 1)
    index = min(int(position), len(FOREARM_PATH) - 2)
    fraction = position - index
    point = Vector(FOREARM_PATH[index]).lerp(Vector(FOREARM_PATH[index + 1]), fraction)
    radius = lerp(FOREARM_RADII[index], FOREARM_RADII[index + 1], fraction)
    if index == 0:
        tangent = Vector(FOREARM_PATH[1]) - Vector(FOREARM_PATH[0])
    else:
        tangent = Vector(FOREARM_PATH[index + 1]) - Vector(FOREARM_PATH[index - 1])
    right, up, _forward = frame_for(tangent)
    return point, radius, right, up


def forearm_bone_for_t(t):
    if t < 0.30:
        return "forearm_01"
    if t < 0.62:
        return "forearm_02"
    return "wrist"


def snap_to_forearm(point):
    """Project a feather base onto the nearest forearm surface point."""
    point = Vector(point)
    best_t, best_distance = 0.0, float("inf")
    for step in range(97):
        t = step / 96.0
        centre, _radius, _right, _up = forearm_sample(t)
        distance = (centre - point).length_squared
        if distance < best_distance:
            best_t, best_distance = t, distance
    centre, radius, right, up = forearm_sample(best_t)
    radial = point - centre
    if radial.length < 1e-6:
        radial = up
    radial.normalize()
    return centre + radial * radius * 0.96


def build_geometry():
    builder = MeshBuilder()

    # The arm continues back into the body; this stump keeps the open end cap out of
    # the first-person frame and gives the forearm a believable attachment.
    stump_path = [(0.112, -0.335, 0.018), (0.118, -0.430, -0.010),
                  (0.120, -0.530, -0.045), (0.118, -0.620, -0.085)]
    tube(builder, stump_path, [0.058, 0.064, 0.060, 0.052], 14, MAT_BLUE,
         lambda t, p: {"forearm_01": 1.0}, uv_scale=1.0, mat_tip=MAT_BLUE)

    # Tapered, ridged forearm. Alternating radii make the hard faceted segments that
    # catch the game's quantized hand shader.
    radii = list(FOREARM_RADII)
    for index in range(2, len(radii) - 2, 3):
        radii[index] *= 1.055
        radii[index + 1] *= 0.975
    tube(builder, FOREARM_PATH, radii, 14, MAT_BLUE, forearm_weights,
         uv_scale=2.0, mat_tip=MAT_BLUE)

    # Layered scale plates on the forearm surface. These are short feather cards
    # weighted rigidly to the arm, giving the hard overlapping plates of the original.
    for plate_index, (t, angle_deg) in enumerate(((0.15, 78), (0.25, 102), (0.35, 76),
                                                  (0.45, 104), (0.55, 80), (0.65, 100))):
        point, radius, right, up = forearm_sample(t)
        angle = math.radians(angle_deg)
        radial = (right * math.cos(angle) + up * math.sin(angle)).normalized()
        base = point + radial * radius * 0.96
        ellipsoid(builder, base, (0.034, 0.046, 0.009),
                  MAT_IRID if plate_index % 3 == 0 else MAT_BLUE,
                  forearm_weights(t, base), rings=5, segments=10)

    # Iridescent scale gems sitting on the top surface.
    for t, angle_deg, gem_size in ((0.31, 96, 0.017), (0.51, 86, 0.014), (0.68, 100, 0.012)):
        point, radius, right, up = forearm_sample(t)
        angle = math.radians(angle_deg)
        radial = (right * math.cos(angle) + up * math.sin(angle)).normalized()
        centre = point + radial * radius * 0.95
        octahedron(builder, centre, gem_size, MAT_IRID, forearm_weights(t, centre))

    # Wrist cuff / scale collar.
    cuff_path = [(0.005, 0.285, 0.109), (0.003, 0.315, 0.107), (0.001, 0.345, 0.102)]
    tube(builder, cuff_path, [0.050, 0.058, 0.052], 14, MAT_BLUE,
         lambda t, p: {"wrist": 1.0}, uv_scale=0.4, mat_tip=MAT_BLUE)

    # Palm.
    ellipsoid(builder, (0.000, 0.402, 0.098), (0.095, 0.115, 0.058), MAT_BLUE,
              {"hand": 1.0}, rings=8, segments=16)
    # Palm pad, a cream plate on the gripping side.
    ellipsoid(builder, (0.000, 0.422, 0.048), (0.076, 0.086, 0.022), MAT_CREAM,
              {"hand": 1.0}, rings=6, segments=12)
    # Knuckle pads.
    for knuckle, radius in (((0.050, 0.450, 0.118), 0.030),
                            ((0.000, 0.458, 0.124), 0.033),
                            ((-0.050, 0.450, 0.118), 0.030)):
        ellipsoid(builder, knuckle, (radius, radius * 1.15, radius * 0.8), MAT_LIGHT,
                  {"hand": 1.0}, rings=5, segments=10)

    fingers = [
        ("finger_a_01", "finger_a_02", [(0.050, 0.440, 0.100), (0.082, 0.490, 0.165),
                                       (0.085, 0.530, 0.225), (0.060, 0.560, 0.270)],
         [0.032, 0.027, 0.019, 0.010], 0.000),
        ("finger_b_01", "finger_b_02", [(0.000, 0.450, 0.105), (0.000, 0.500, 0.180),
                                       (0.000, 0.535, 0.250), (0.000, 0.560, 0.300)],
         [0.036, 0.030, 0.021, 0.011], 0.000),
        ("finger_c_01", "finger_c_02", [(-0.050, 0.440, 0.100), (-0.082, 0.490, 0.165),
                                       (-0.085, 0.530, 0.225), (-0.060, 0.560, 0.270)],
         [0.031, 0.026, 0.018, 0.009], 0.000),
    ]
    for bone_a, bone_b, path, radii_f, curve in fingers:
        points = [Vector(point) for point in path]
        tube(builder, points, radii_f, 10, MAT_BLUE,
             lambda t, p, a=bone_a, b=bone_b: finger_weights(a, b, t),
             uv_scale=1.0, mat_tip=MAT_BLUE)
        for joint_index in (1, 2):
            joint_t = joint_index / (len(points) - 1)
            joint_radius = radii_f[joint_index] * 1.08
            ellipsoid(builder, points[joint_index],
                      (joint_radius * 1.12, joint_radius * 1.30, joint_radius * 1.05),
                      MAT_BLUE, finger_weights(bone_a, bone_b, joint_t),
                      rings=5, segments=10)
        tip = points[-1]
        direction = (points[-1] - points[-2]).normalized()
        claw_mid = tip + direction * 0.028 + Vector((0.0, -0.004, 0.006))
        claw_end = tip + direction * 0.050 + Vector((0.0, -0.014, 0.004))
        tube(builder, [tip, claw_mid, claw_end], [0.011, 0.007, 0.001], 7, MAT_CLAW,
             lambda t, p, a=bone_a, b=bone_b: finger_weights(a, b, t),
             uv_scale=1.0, mat_tip=MAT_CLAW)

    # Thumb.
    thumb_path = [(-0.050, 0.390, 0.090), (-0.100, 0.410, 0.140),
                  (-0.080, 0.445, 0.190), (-0.020, 0.470, 0.215)]
    tube(builder, thumb_path, [0.025, 0.019, 0.013, 0.008], 10, MAT_BLUE,
         lambda t, p: finger_weights("thumb_01", "thumb_02", t), uv_scale=1.0,
         mat_tip=MAT_BLUE)
    thumb_tip = Vector(thumb_path[-1])
    thumb_dir = (Vector(thumb_path[-1]) - Vector(thumb_path[-2])).normalized()
    tube(builder, [thumb_tip, thumb_tip + thumb_dir * 0.024 + Vector((0.0, -0.004, 0.006)),
                   thumb_tip + thumb_dir * 0.046 + Vector((0.0, -0.014, 0.004))],
         [0.011, 0.007, 0.001], 7, MAT_CLAW,
         lambda t, p: finger_weights("thumb_01", "thumb_02", t),
         uv_scale=1.0, mat_tip=MAT_CLAW)

    # Layered feathers. Each row has its own control bone, and its feathers share a
    # family so the runtime can animate a whole row cheaply while the cards overlap.
    rows = [
        # bone, count, length range, width, spread, sweep, lift, parent
        ("feather_up_01", 4, (0.180, 0.240), (0.050, 0.070), 0.075, (0.10, 0.16, 0.98), 0.00, "forearm_01"),
        ("feather_up_02", 4, (0.200, 0.265), (0.055, 0.075), 0.080, (0.08, 0.18, 0.98), 0.00, "forearm_02"),
        ("feather_up_03", 3, (0.170, 0.225), (0.050, 0.070), 0.070, (0.06, 0.20, 0.98), 0.00, "wrist"),
        ("feather_outer_01", 3, (0.160, 0.215), (0.050, 0.070), 0.065, (0.90, 0.16, 0.40), 0.00, "forearm_01"),
        ("feather_outer_02", 3, (0.170, 0.230), (0.052, 0.072), 0.070, (0.92, 0.18, 0.36), 0.00, "forearm_02"),
        ("feather_inner_01", 2, (0.120, 0.170), (0.045, 0.065), 0.060, (-0.90, 0.16, 0.40), 0.00, "forearm_01"),
        ("feather_inner_02", 2, (0.130, 0.180), (0.048, 0.068), 0.065, (-0.92, 0.18, 0.36), 0.00, "forearm_02"),
        ("feather_primary", 4, (0.300, 0.400), (0.060, 0.080), 0.075, (0.08, 0.06, 1.00), 0.00, "wrist"),
        ("feather_alula", 2, (0.120, 0.170), (0.045, 0.060), 0.055, (0.42, 0.12, 0.90), 0.00, "wrist"),
    ]

    bone_heads = {name: Vector(head) for name, head, _tail, _parent in BONES}
    for bone_name, count, length_range, width_range, spread, sweep, lift, parent in rows:
        base_anchor = bone_heads[bone_name]
        sweep = Vector(sweep).normalized()
        side = safe_normal(sweep.cross(Vector((0.0, 0.0, 1.0))), Vector((1.0, 0.0, 0.0)))
        for index in range(count):
            frac = 0.0 if count == 1 else (index / (count - 1) - 0.5)
            base = snap_to_forearm(base_anchor + side * (frac * spread)
                                   + Vector((0.0, 0.0, lift * abs(frac))))
            direction = (sweep + side * (frac * 0.35)
                         + Vector((0.0, 0.0, RNG.uniform(-0.05, 0.05)))).normalized()
            length = RNG.uniform(*length_range)
            width = RNG.uniform(*width_range)
            material = MAT_IRID if index % 5 == 0 else MAT_BLUE
            tip_material = MAT_IRID if index % 6 == 0 else MAT_BLUE
            parent_weight = 0.28 if parent else None
            feather(builder, base, direction, length, width,
                    curve=RNG.uniform(-0.02, 0.035), bone=bone_name,
                    material=material, tip_material=tip_material,
                    side_hint=Vector((0.0, -1.0, 1.0)),
                    thickness=0.0024, parent_bone=parent, twist=RNG.uniform(-0.18, 0.18))
            # A second, shorter covert layer in front of the main feather gives depth
            # without doubling the bone count.
            if index % 2 == 0:
                feather(builder, base + direction * 0.028 + side * 0.012,
                        direction + Vector((0.0, 0.0, 0.10)), length * 0.55, width * 0.72,
                        curve=-0.012, bone=bone_name, material=MAT_BLUE,
                        tip_material=MAT_BLUE, thickness=0.0020,
                        side_hint=Vector((0.0, -1.0, 1.0)),
                        parent_bone=parent, twist=RNG.uniform(-0.15, 0.15))

    # Small wrist coverts, layered over the cuff.
    for index in range(6):
        angle = -0.9 + index * 0.36
        base = Vector((math.cos(angle) * 0.050, 0.300 + 0.012 * math.sin(angle),
                       0.108 + math.sin(angle) * 0.040))
        direction = Vector((math.cos(angle) * 0.55, -0.75, math.sin(angle) * 0.45))
        feather(builder, base, direction, 0.105 + 0.02 * (index % 2), 0.042,
                curve=0.01, bone="feather_up_03", material=MAT_BLUE,
                tip_material=MAT_LIGHT, thickness=0.0022, parent_bone="wrist",
                twist=0.1 * (index - 2))

    return builder


# --------------------------------------------------------------------------- simulation


class Spring:
    def __init__(self, stiffness, damping, limit):
        self.stiffness = stiffness
        self.damping = damping
        self.limit = limit
        self.angle = 0.0
        self.velocity = 0.0

    def step(self, target, dt):
        target = max(-self.limit, min(self.limit, target))
        # Sub-step so the response is independent of the bake frame rate.
        remaining = dt
        while remaining > 1e-9:
            h = min(remaining, 1.0 / 240.0)
            acceleration = self.stiffness * (target - self.angle) - self.damping * self.velocity
            self.velocity += acceleration * h
            self.angle += self.velocity * h
            if abs(self.angle) > self.limit * 1.35:
                self.angle = math.copysign(self.limit * 1.35, self.angle)
                self.velocity *= -0.15
            remaining -= h
        return self.angle

    def reset(self):
        self.angle = 0.0
        self.velocity = 0.0


def action_profile(name):
    """Return duration, per-frame arm pose, and a world-motion profile for the spring."""
    if name == "idle":
        return dict(frames=48, arm=lambda t: (
            (0.012 * math.sin(t * math.tau), 0.0, 0.012 * math.sin(t * math.tau + 0.8)),
            (0.0, 0.010 * math.sin(t * math.tau + 1.2), 0.020 * math.sin(t * math.tau)),
        ), motion=lambda t: (0.0, 0.03 * math.sin(t * math.tau), 0.02 * math.sin(t * math.tau * 2.0),
                             0.0, 0.03, 0.0))
    if name == "dash":
        return dict(frames=36, arm=lambda t: (
            (-0.22 * smooth(0.05, 0.35, t), 0.0, -0.15 * smooth(0.05, 0.35, t)),
            (0.10 * smooth(0.1, 0.45, t), 0.0, 0.18 * smooth(0.05, 0.4, t)),
        ), motion=lambda t: (0.0, 9.0 * smooth(0.0, 0.22, t) - 2.0 * smooth(0.45, 1.0, t),
                             0.0, 0.0, 14.0 if 0.12 < t < 0.3 else 0.0, 0.0))
    if name == "dash_slide":
        return dict(frames=48, arm=lambda t: (
            (0.10, 0.30 * smooth(0.0, 0.25, t), -0.24 * smooth(0.0, 0.25, t)),
            (-0.30 * smooth(0.05, 0.4, t), 0.0, 0.08),
        ), motion=lambda t: (0.0, 7.5 * smooth(0.0, 0.2, t) - 4.0 * smooth(0.5, 1.0, t),
                             -0.8 * smooth(0.0, 0.2, t), 0.0, 9.0 if 0.1 < t < 0.25 else 0.0, 0.0))
    if name == "jump":
        return dict(frames=36, arm=lambda t: (
            (0.16 * smooth(0.1, 0.4, t), 0.0, 0.05),
            (0.12 * smooth(0.1, 0.5, t), 0.0, -0.06 * smooth(0.1, 0.5, t)),
        ), motion=lambda t: (0.0, 2.0 * smooth(0.0, 0.2, t), 7.0 * smooth(0.05, 0.25, t)
                             - 3.0 * smooth(0.55, 1.0, t),
                             0.0, 0.0, 10.0 if 0.06 < t < 0.26 else 0.0))
    if name == "stomp":
        return dict(frames=36, arm=lambda t: (
            (-0.25 * smooth(0.0, 0.25, t), 0.0, 0.0),
            (-0.30 * smooth(0.05, 0.35, t), 0.0, 0.0),
        ), motion=lambda t: (0.0, 0.0, -8.0 * smooth(0.05, 0.25, t),
                             0.0, 0.0, -16.0 if 0.18 < t < 0.32 else 0.0))
    if name == "bunnyhop":
        return dict(frames=60, arm=lambda t: (
            (0.05 * math.sin(t * math.tau * 2.0), 0.0, 0.03 * math.sin(t * math.tau)),
            (0.06 * math.sin(t * math.tau * 2.0 + 0.4), 0.0, 0.0),
        ), motion=lambda t: (0.0, 2.5, 5.0 * math.sin(t * math.tau * 2.0),
                             0.0, 0.0, 12.0 * math.cos(t * math.tau * 2.0)))
    if name == "fake":
        return dict(frames=36, arm=lambda t: (
            (0.0, 0.0, 0.30 * math.sin(math.pi * min(1.0, t * 1.4))),
            (0.0, 0.0, -0.20 * math.sin(math.pi * min(1.0, t * 1.4))),
        ), motion=lambda t: (5.0 * math.sin(math.pi * min(1.0, t * 1.2)),
                             2.0, 0.0, 8.0 if 0.1 < t < 0.3 else 0.0, 0.0, 0.0))
    raise KeyError(name)


def reset_pose(arm_obj):
    for pose_bone in arm_obj.pose.bones:
        pose_bone.rotation_euler = (0.0, 0.0, 0.0)
        pose_bone.location = (0.0, 0.0, 0.0)
        pose_bone.scale = (1.0, 1.0, 1.0)


def bake_actions(arm_obj):
    action_names = ["idle", "dash", "dash_slide", "jump", "stomp", "bunnyhop", "fake"]
    baked = {}
    for name in action_names:
        profile = action_profile(name)
        frames = profile["frames"]
        action = bpy.data.actions.new(f"berdly_{name}")
        arm_obj.animation_data_create()
        arm_obj.animation_data.action = action
        reset_pose(arm_obj)
        springs = {}
        sampled = {}
        for index, bone in enumerate(FEATHER_BONES):
            # Outer and primary rows are softer and lag more; the chain is intentionally
            # hierarchical rather than 31 independent feathers.
            stiffness = 48.0 if "primary" in bone or "outer" in bone else 64.0
            damping = 7.0 if "primary" in bone or "outer" in bone else 9.0
            springs[bone] = (Spring(stiffness, damping, 0.95),
                             Spring(stiffness * 0.82, damping * 1.05, 0.70), 0.0)
        for frame in range(1, frames + 1):
            t = (frame - 1) / max(1, frames - 1)
            motion = profile["motion"](t)
            velocity = Vector(motion[0:3])
            acceleration = Vector(motion[3:6])
            arm_pair = profile["arm"](t)
            forearm_rot, wrist_rot = arm_pair
            # Convert world airflow into arm-local space by rotating with the arm pose.
            rot = Euler((forearm_rot[0], forearm_rot[1], forearm_rot[2]), "XYZ").to_matrix()
            local_velocity = rot.transposed() @ velocity
            local_acceleration = rot.transposed() @ acceleration
            for bone_index, bone in enumerate(FEATHER_BONES):
                sweep_spring, flare_spring, phase = springs[bone]
                phase += 1.0 / 60.0
                springs[bone] = (sweep_spring, flare_spring, phase)
                lag = 0.82 + 0.06 * (bone_index % 4)
                sweep_target = (-local_velocity.y * 0.075 * lag
                                - local_acceleration.y * 0.0055 * lag
                                - local_velocity.z * 0.030 * lag)
                flare_target = (local_velocity.x * 0.055 * lag
                                + local_acceleration.x * 0.004 * lag
                                + local_velocity.y * 0.010 * lag)
                sweep = sweep_spring.step(sweep_target, 1.0 / 24.0)
                flare = flare_spring.step(flare_target, 1.0 / 24.0)
                pose_bone = arm_obj.pose.bones[bone]
                pose_bone.rotation_euler = (sweep, 0.0, flare)
                pose_bone.keyframe_insert("rotation_euler", frame=frame)
                sampled.setdefault(bone, []).append([sweep, 0.0, flare])
            arm_obj.pose.bones["forearm_01"].rotation_euler = forearm_rot
            arm_obj.pose.bones["forearm_02"].rotation_euler = (
                forearm_rot[0] * 0.6, 0.0, forearm_rot[2] * 1.2)
            arm_obj.pose.bones["wrist"].rotation_euler = wrist_rot
            arm_obj.pose.bones["hand"].rotation_euler = (
                wrist_rot[0] * 0.6, 0.0, wrist_rot[2] * 0.4)
            for bone in ("forearm_01", "forearm_02", "wrist", "hand"):
                arm_obj.pose.bones[bone].keyframe_insert("rotation_euler", frame=frame)
                sampled.setdefault(bone, []).append(
                    list(arm_obj.pose.bones[bone].rotation_euler))
            for bone in ("finger_a_02", "finger_b_02", "finger_c_02", "thumb_02"):
                curl = 0.06 * math.sin(t * math.pi * 2.0) + 0.04
                arm_obj.pose.bones[bone].rotation_euler = (0.0, 0.0, -curl)
                arm_obj.pose.bones[bone].keyframe_insert("rotation_euler", frame=frame)
                sampled.setdefault(bone, []).append([0.0, 0.0, -curl])
        action.use_fake_user = True
        baked[name] = (action, frames, sampled)
    reset_pose(arm_obj)
    arm_obj.animation_data.action = baked["idle"][0]
    return baked


# --------------------------------------------------------------------------- scene / render


def setup_scene():
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    try:
        scene.eevee.taa_render_samples = 32
    except Exception:
        pass
    scene.render.resolution_x = 900
    scene.render.resolution_y = 506
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.render.image_settings.file_format = "PNG"
    scene.world = bpy.data.worlds.new("BerdlyWorld")
    scene.world.use_nodes = True
    background = scene.world.node_tree.nodes.get("Background")
    if background:
        background.inputs["Color"].default_value = (0.006, 0.008, 0.018, 1.0)
        background.inputs["Strength"].default_value = 0.35
    try:
        scene.view_settings.look = "AgX - Medium High Contrast"
        scene.view_settings.exposure = -0.35
    except Exception:
        pass

    def area(name, location, energy, colour, size, rotation):
        data = bpy.data.lights.new(name, "AREA")
        data.energy = energy
        data.color = colour
        data.size = size
        obj = bpy.data.objects.new(name, data)
        obj.location = location
        obj.rotation_euler = rotation
        bpy.context.scene.collection.objects.link(obj)
        return obj

    area("Key", (0.55, -0.85, 0.95), 38.0, (0.55, 0.74, 1.0), 1.1,
         (math.radians(52), 0.0, math.radians(38)))
    area("Rim", (-0.75, 0.45, 0.55), 26.0, (0.10, 0.55, 1.00), 0.9,
         (math.radians(68), 0.0, math.radians(-128)))
    area("Fill", (0.10, 0.65, 0.10), 12.0, (0.16, 0.65, 1.0), 1.4,
         (math.radians(110), 0.0, math.radians(178)))
    area("Under", (0.0, -0.10, -0.65), 8.0, (0.18, 0.32, 1.0), 0.8,
         (math.radians(-55), 0.0, 0.0))

    camera_data = bpy.data.cameras.new("BerdlyCam")
    camera = bpy.data.objects.new("BerdlyCam", camera_data)
    bpy.context.scene.collection.objects.link(camera)
    scene.camera = camera
    camera_data.lens = 34.0
    camera_data.clip_start = 0.01
    camera_data.clip_end = 100.0
    return camera


def look_at(camera, location, target):
    camera.location = location
    direction = Vector(target) - Vector(location)
    camera.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def render_views(camera, baked):
    RENDERS.mkdir(parents=True, exist_ok=True)
    scene = bpy.context.scene
    fp = ((0.120, -0.980, 0.300), (0.120, 0.320, 0.100))
    fp_wide = ((0.130, -1.130, 0.340), (0.120, 0.320, 0.090))
    fp_hand = ((0.240, -0.780, 0.340), (0.060, 0.420, 0.200))
    side = ((0.760, -0.060, 0.430), (0.010, 0.230, 0.130))
    three = ((0.560, -0.400, 0.430), (0.010, 0.260, 0.140))
    close = ((0.180, -0.140, 0.380), (0.020, 0.280, 0.170))
    shots = [
        ("fp_idle", "idle", 14, fp),
        ("fp_wide", "idle", 14, fp_wide),
        ("fp_hand", "idle", 14, fp_hand),
        ("fp_dash", "dash", 12, fp),
        ("fp_dash_slide", "dash_slide", 18, fp),
        ("fp_jump", "jump", 12, fp),
        ("fp_stomp", "stomp", 10, fp),
        ("fp_bunnyhop", "bunnyhop", 22, fp),
        ("fp_fake", "fake", 12, fp),
        ("side_idle", "idle", 14, side),
        ("three_dash", "dash", 12, three),
        ("feather_close", "dash", 12, close),
    ]
    for name, action_name, frame, (location, target) in shots:
        action, _frames, _sampled = baked[action_name]
        arm = bpy.data.objects["BerdlyArmRig"]
        bpy.context.view_layer.objects.active = arm
        arm.animation_data.action = action
        scene.frame_start = 1
        scene.frame_end = _frames
        scene.frame_set(frame)
        look_at(camera, Vector(location), Vector(target))
        scene.render.filepath = str(RENDERS / f"{name}.png")
        bpy.ops.render.render(write_still=True)
        print(f"rendered {name}")


# --------------------------------------------------------------------------- export


def export_runtime(baked):
    arm = bpy.data.objects["BerdlyArmRig"]
    mesh_obj = bpy.data.objects["BerdlyArmMesh"]
    mesh = mesh_obj.data
    arm.data.pose_position = "REST"
    bpy.context.view_layer.update()

    vertices = [list(v.co) for v in mesh.vertices]
    normals = [list(v.normal) for v in mesh.vertices]
    uv_layer = mesh.uv_layers.active
    faces = []
    face_uvs = []
    for polygon in mesh.polygons:
        faces.append(list(polygon.vertices))
        face_uvs.append([[uv_layer.data[li].uv[0], uv_layer.data[li].uv[1]]
                         for li in polygon.loop_indices])
    groups = {group.index: group.name for group in mesh_obj.vertex_groups}
    weights = []
    for vertex in mesh.vertices:
        entries = sorted(((entry.weight, groups[entry.group]) for entry in vertex.groups
                          if entry.weight > 1e-4), reverse=True)[:4]
        total = sum(weight for weight, _ in entries) or 1.0
        weights.append([[name, weight / total] for weight, name in entries])
    bones = []
    for bone in arm.data.bones:
        bones.append({
            "name": bone.name,
            "parent": bone.parent.name if bone.parent else None,
            "head": list(bone.head_local),
            "tail": list(bone.tail_local),
            "matrix": [list(row) for row in bone.matrix_local],
        })
    actions = {}
    for name, (action, frames, sampled) in baked.items():
        # Blender 5.x actions no longer expose fcurves directly (layered actions). The
        # bake already has the exact per-frame Euler samples, which is also what the
        # runtime needs, so export those rather than reverse-engineering action layers.
        actions[name] = {"frames": frames, "rotation_euler_samples": sampled}

    payload = {
        "schema": "hyprlodr.berdly-arm/1",
        "generated_by": "tools/assets/build_berdly_arm.py",
        "units": "metres; +Y forward, +Z up, +X right (rig space)",
        "mesh": {
            "name": mesh.name,
            "vertices": vertices,
            "normals": normals,
            "faces": faces,
            "face_uvs": face_uvs,
            "weights": weights,
            "materials": [slot.material.name for slot in mesh_obj.material_slots],
            "face_materials": [polygon.material_index for polygon in mesh.polygons],
        },
        "armature": {"name": arm.data.name, "bones": bones},
        "feather_bones": FEATHER_BONES,
        "springs": {
            "substep_hz": 240,
            "sweep_stiffness": 64.0,
            "sweep_damping": 9.0,
            "flare_stiffness": 52.0,
            "flare_damping": 9.5,
            "sweep_limit": 0.95,
            "flare_limit": 0.70,
            "airflow_gain": {"sweep_y": 0.075, "sweep_z": 0.030, "flare_x": 0.055},
            "states": ["idle", "dash", "dash_slide", "jump", "stomp", "bunnyhop", "fake"],
        },
        "actions": actions,
        "game_material_targets": [
            "entity_hand", "entity_hand_vs", "entity_hand_position", "entity_hand_cns",
            "entity_veins_cns", "hand1bones", "hand1fract", "hand2bones", "hand2fract",
        ],
        "bone_budget": 16,
        "authoring_bone_count": len(bones),
        "runtime_skinning_status": "pending-16-bone-bake",
        "notes": [
            "The authoring armature has 26 bones (fingers, claws and nine feather rows). "
            "The verified entity path supplies at most 16 bone matrices, so a 16-bone "
            "runtime bake is required before the mesh can use the game draw path.",
            "The rig is authored for the verified entity 16-bone LBS path.",
            "The Blender materials are an art target; runtime keeps entity_hand shading.",
            "Feather bones are hierarchical rows, not 31 independent simulators.",
        ],
    }
    (OUT / "berdly_arm.mesh.json").write_text(json.dumps(payload, indent=1))
    print(f"wrote {OUT / 'berdly_arm.mesh.json'}")

    bpy.ops.object.select_all(action="DESELECT")
    arm.select_set(True)
    mesh_obj.select_set(True)
    bpy.context.view_layer.objects.active = arm
    try:
        bpy.ops.export_scene.gltf(filepath=str(OUT / "berdly_arm.glb"),
                                  export_format="GLB", use_selection=True,
                                  export_animations=True)
        print("wrote GLB")
    except Exception as error:
        print(f"GLB export skipped: {error}")
    arm.data.pose_position = "POSE"


# --------------------------------------------------------------------------- main


def main():
    quick = "--no-render" in sys.argv
    for collection in (bpy.data.objects, bpy.data.meshes, bpy.data.armatures,
                       bpy.data.materials, bpy.data.actions):
        for item in list(collection):
            collection.remove(item)
    OUT.mkdir(parents=True, exist_ok=True)

    materials = [
        add_material("BerdlyBlue", BLUE, metallic=0.35, roughness=0.40,
                     iridescence=0.22, emission=0.015, ramp="prism"),
        add_material("BerdlyLight", BLUE_LIGHT, metallic=0.28, roughness=0.34,
                     iridescence=0.32, emission=0.055, ramp="prism"),
        add_material("BerdlyCream", CREAM, metallic=0.05, roughness=0.55,
                     iridescence=0.08, emission=0.0, ramp="cream"),
        add_material("BerdlyClaw", CLAW, metallic=0.18, roughness=0.46,
                     iridescence=0.08, emission=0.0, ramp="prism"),
        add_material("BerdlyIridescent", (0.010, 0.075, 0.400, 1.0), metallic=0.55,
                     roughness=0.24, iridescence=0.52, emission=0.04, ramp="prism"),
    ]

    builder = build_geometry()
    mesh_obj = builder.to_object("BerdlyArmMesh", materials)
    arm_obj = create_rig()

    modifier = mesh_obj.modifiers.new("Armature", "ARMATURE")
    modifier.object = arm_obj
    mesh_obj.parent = arm_obj

    # Faceted shading: the original arm is hard, quantized and plate-like.
    for polygon in mesh_obj.data.polygons:
        polygon.use_smooth = False

    baked = bake_actions(arm_obj)
    camera = setup_scene()

    if not quick:
        render_views(camera, baked)

    # Save the editable project first: the .blend is a deliverable even if a viewer export
    # is unavailable in a particular Blender build.
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT / "berdly_arm.blend"))
    print(f"saved {OUT / 'berdly_arm.blend'}")
    export_runtime(baked)
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT / "berdly_arm.blend"))


if __name__ == "__main__":
    main()
