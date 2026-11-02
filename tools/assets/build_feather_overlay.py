#!/usr/bin/env python3
"""Attach authored feather cards to the extracted HYPER DEMON hand mesh.

The original arm buffer is already posed and carries no skinning data, so the
feathers cannot be rigged to hidden bones. Instead each feather vertex is bound
to the four nearest original vertices. At runtime the live hand VBO supplies
those vertices' current positions, and the feather follows the exact same
deformation as the arm it grows from. This preserves the original animation and
material path instead of replacing the hand rig.

Inputs are local extracts of the user's own installation:
    assets/hand-extract/original_arm.blend   (tools/assets/import_hand_vbo.py)

Outputs (ignored by Git):
    assets/hand-extract/original_arm_feathered.blend
    assets/hand-extract/feather_overlay.bin
    assets/hand-extract/feathered_arm_{fp,side,top}.png

Binary layout, little-endian:
    0   magic       8 bytes  b"HYPRFEAT"
    8   version     u32      1
    12  vertex_count u32
    16  index_count  u32
    20  bind_count   u32      4
    24  reserved    2 x u32
    32  vertices     vertex_count * 64 bytes:
            position 3f, normal 3f, uv 2f,
            bind indices 4 x u32, bind weights 4f
        indices      index_count * u32

Usage:
    blender -b assets/hand-extract/original_arm.blend \
        --python tools/assets/build_feather_overlay.py
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
from pathlib import Path

import bpy
from mathutils import Vector
from mathutils.kdtree import KDTree

ROOT = Path(__file__).resolve().parents[2]
EXTRACT = ROOT / "assets" / "hand-extract"
BIND_COUNT = 4
MAGIC = b"HYPRFEAT"
VERSION = 1


def arguments() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=EXTRACT)
    parser.add_argument("--resolution", type=int, default=960)
    return parser.parse_args(argv)


def material(name: str, colour, metallic: float, roughness: float):
    mat = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    if bsdf:
        bsdf.inputs["Base Color"].default_value = (*colour, 1.0)
        bsdf.inputs["Metallic"].default_value = metallic
        bsdf.inputs["Roughness"].default_value = roughness
        coat = bsdf.inputs.get("Coat Weight")
        if coat:
            coat.default_value = 0.25
    return mat


def look_at(obj, location, target):
    obj.location = Vector(location)
    obj.rotation_euler = (Vector(target) - Vector(location)).to_track_quat("-Z", "Y").to_euler()


def add_camera(name, location, target, lens):
    data = bpy.data.cameras.new(name)
    data.lens = lens
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    look_at(obj, location, target)
    return obj


def cross_section_center(source, y: float) -> Vector:
    near = [v.co for v in source.data.vertices if abs(v.co.y - y) < 0.026]
    if len(near) < 4:
        near = [v.co for v in source.data.vertices]
    return sum(near, Vector()) / len(near)


def surface_hit(source, center: Vector, angle: float):
    radial = Vector((math.cos(angle), 0.0, math.sin(angle)))
    origin = center + radial * 0.35
    hit, location, normal, _face = source.ray_cast(origin, -radial, distance=0.8)
    if not hit:
        return None
    if normal.dot(radial) < 0:
        normal = -normal
    return location, normal.normalized()


def make_card(root: Vector, normal: Vector, direction: Vector, length: float,
              width: float):
    """A tapered vane plus a narrow, raised gold rachis ribbon."""
    direction = direction.normalized()
    side = direction.cross(normal)
    if side.length < 1e-5:
        side = Vector((1, 0, 0))
    side.normalize()
    segments = 3
    vane_points: list[Vector] = []
    vane_uvs: list[tuple[float, float]] = []
    rib_points: list[Vector] = []
    rib_uvs: list[tuple[float, float]] = []
    for segment in range(segments + 1):
        t = segment / segments
        centre = root + direction * (length * t) + normal * (0.0035 * math.sin(math.pi * t))
        centre -= normal * (0.004 * t * t)
        half = width * (0.30 + 0.70 * math.sin(math.pi * (0.18 + 0.82 * t))) * (1.0 - 0.42 * t)
        if 0.55 < t < 0.9:
            half *= 1.05
        left = centre - side * half
        right = centre + side * half
        raised = centre + normal * 0.0018
        v = 0.08 + 0.88 * t
        vane_points.extend((left, raised, right))
        vane_uvs.extend(((0.24, v), (0.86, v), (0.24, v)))
        rib_left = centre - side * 0.0012 + normal * 0.0028
        rib_right = centre + side * 0.0012 + normal * 0.0028
        rib_points.extend((rib_left, rib_right))
        rib_uvs.extend(((0.86, v), (0.86, v)))
    vane_faces = []
    for segment in range(segments):
        base = segment * 3
        vane_faces.append((base + 0, base + 1, base + 4, base + 3))
        vane_faces.append((base + 1, base + 2, base + 5, base + 4))
    rib_faces = []
    for segment in range(segments):
        base = segment * 2
        rib_faces.append((base + 0, base + 1, base + 3, base + 2))
    return vane_points, vane_uvs, vane_faces, rib_points, rib_uvs, rib_faces


def build_feathers(source):
    vertices: list[Vector] = []
    uvs: list[tuple[float, float]] = []
    faces: list[tuple[int, int]] = []  # (vertex tuple length 4, material)
    cards = []

    def add_card(root, normal, direction, length, width):
        card = make_card(root, normal, direction, length, width)
        vane_points, vane_uvs, vane_faces, rib_points, rib_uvs, rib_faces = card
        base = len(vertices)
        vertices.extend(vane_points)
        uvs.extend(vane_uvs)
        for face in vane_faces:
            faces.append((tuple(base + i for i in face), 0))
        rib_base = len(vertices)
        vertices.extend(rib_points)
        uvs.extend(rib_uvs)
        for face in rib_faces:
            faces.append((tuple(rib_base + i for i in face), 1))
        cards.append(card)

    # Forearm coverts: four overlapping rows on the upper/outer half. Keep the
    # cards short and sparse so they read as a coat instead of a full shell.
    stations = (-0.165, -0.120, -0.075, -0.030)
    angles = (0.55, 1.10, 1.65, 4.55, 5.10)
    for row, y in enumerate(stations):
        center = cross_section_center(source, y)
        for column, angle in enumerate(angles):
            hit = surface_hit(source, center, angle)
            if hit is None:
                continue
            location, normal = hit
            direction = Vector((0.0, 0.94, 0.0)) + normal * 0.16
            length = 0.034 + 0.004 * ((row + column) % 3)
            width = 0.010 + 0.0015 * ((row + column) % 2)
            add_card(location + normal * 0.0035, normal, direction, length, width)

    # Wrist primaries sweep back over the forearm; they are longer and narrower
    # than the coverts and leave the hand itself clear.
    center = cross_section_center(source, -0.010)
    for index, angle in enumerate((0.45, 1.05, 1.65, 4.55, 5.15)):
        hit = surface_hit(source, center, angle)
        if hit is None:
            continue
        location, normal = hit
        direction = Vector((0.0, -0.96, 0.0)) + normal * 0.15
        length = 0.080 + 0.008 * (index % 3)
        width = 0.014 + 0.0015 * (index % 2)
        add_card(location + normal * 0.0050, normal, direction, length, width)

    # Small coverts on the back of the hand.
    for index, angle in enumerate((1.25, 4.75)):
        center = cross_section_center(source, 0.050 + 0.018 * index)
        hit = surface_hit(source, center, angle)
        if hit is None:
            continue
        location, normal = hit
        direction = Vector((0.0, 0.86, 0.0)) + normal * 0.28
        add_card(location + normal * 0.0035, normal, direction, 0.026, 0.0085)

    triangles: list[tuple[int, int, int]] = []
    triangle_materials: list[int] = []
    for face, material_index in faces:
        a, b, c, d = face
        triangles.extend(((a, b, c), (a, c, d)))
        triangle_materials.extend((material_index, material_index))

    mesh = bpy.data.meshes.new("FeatherOverlayMesh")
    mesh.from_pydata([tuple(point) for point in vertices], [], triangles)
    mesh.update()
    uv_layer = mesh.uv_layers.new(name="uv0")
    for loop in mesh.loops:
        uv_layer.data[loop.index].uv = uvs[loop.vertex_index]
    blue = material("FeatherBlue", (0.020, 0.095, 0.50), 0.35, 0.34)
    gold = material("FeatherGold", (0.82, 0.50, 0.045), 0.80, 0.22)
    mesh.materials.append(blue)
    mesh.materials.append(gold)
    for index, polygon in enumerate(mesh.polygons):
        polygon.material_index = triangle_materials[index]
        polygon.use_smooth = False

    overlay = bpy.data.objects.new("BerdlyFeatherOverlay", mesh)
    bpy.context.collection.objects.link(overlay)
    return overlay, len(cards)


def bind_vertices(vertices: list[Vector], kd: KDTree):
    indices: list[list[int]] = []
    weights: list[list[float]] = []
    for point in vertices:
        nearest = kd.find_n(point, BIND_COUNT)
        if not nearest:
            raise RuntimeError("empty KD tree")
        raw = [(index, 1.0 / (distance + 0.004)) for _co, index, distance in nearest]
        total = sum(weight for _index, weight in raw)
        indices.append([index for index, _weight in raw])
        weights.append([weight / total for _index, weight in raw])
    return indices, weights


def export_binary(path: Path, obj: bpy.types.Object, bind_indices, bind_weights) -> dict:
    mesh = obj.data
    mesh.calc_loop_triangles()
    vertices = [v.co.copy() for v in mesh.vertices]
    normals = [v.normal.copy() for v in mesh.vertices]
    uvs = [(0.24, 0.5)] * len(vertices)
    if mesh.uv_layers:
        layer = mesh.uv_layers.active.data
        seen = set()
        for loop in mesh.loops:
            if loop.vertex_index not in seen:
                uvs[loop.vertex_index] = tuple(layer[loop.index].uv)
                seen.add(loop.vertex_index)
    indices = []
    for triangle in mesh.loop_triangles:
        indices.extend(triangle.vertices)
    if len(indices) % 3 or not indices:
        raise RuntimeError("overlay mesh has no triangles")

    data = bytearray(struct.pack("<8sIIIIII", MAGIC, VERSION, len(vertices),
                                 len(indices), BIND_COUNT, 0, 0))
    for index, (position, normal, uv) in enumerate(zip(vertices, normals, uvs)):
        data.extend(struct.pack("<3f3f2f4I4f", *position, *normal, *uv,
                                *bind_indices[index], *bind_weights[index]))
    data.extend(struct.pack(f"<{len(indices)}I", *indices))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return {
        "vertices": len(vertices),
        "indices": len(indices),
        "triangles": len(indices) // 3,
        "bytes": len(data),
        "max_bind_weight_error": max(abs(sum(row) - 1.0) for row in bind_weights),
        "used_original_vertices": len({i for row in bind_indices for i in row}),
    }


def render(path: Path, resolution: int, source) -> None:
    scene = bpy.context.scene
    try:
        scene.render.engine = "BLENDER_EEVEE_NEXT"
    except TypeError:
        scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = resolution
    scene.render.resolution_y = int(resolution * 2 / 3)
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.world = bpy.data.worlds.get("HandWorld") or bpy.data.worlds.new("HandWorld")
    scene.world.use_nodes = True
    background = scene.world.node_tree.nodes.get("Background")
    if background:
        background.inputs["Color"].default_value = (0.006, 0.008, 0.014, 1.0)
        background.inputs["Strength"].default_value = 0.30
    for light in bpy.data.lights:
        if light.name in {"Key", "Fill", "Rim"}:
            light.energy *= 0.55

    # Preview the target colour without overwriting the extracted gold mesh in
    # the saved blend.
    preview = source.copy()
    preview.data = source.data.copy()
    preview.name = "OriginalHandBluePreview"
    bpy.context.collection.objects.link(preview)
    preview.data.materials.clear()
    preview.data.materials.append(material("HandBluePreview", (0.018, 0.070, 0.38), 0.55, 0.30))
    for polygon in preview.data.polygons:
        polygon.material_index = 0
    source.hide_render = True

    path.mkdir(parents=True, exist_ok=True)
    for name, location, target, lens in (
        ("feathered_arm_fp", (0.10, -0.70, 0.10), (0.00, 0.06, 0.02), 55.0),
        ("feathered_arm_side", (0.58, -0.30, 0.26), (0.00, 0.00, 0.02), 62.0),
        ("feathered_arm_top", (0.02, -0.18, 0.68), (0.00, 0.02, 0.00), 62.0),
    ):
        camera = add_camera(name, location, target, lens)
        scene.camera = camera
        scene.render.filepath = str(path / f"{name}.png")
        bpy.ops.render.render(write_still=True)
        print(f"rendered: {scene.render.filepath}")


def main() -> int:
    args = arguments()
    source = bpy.data.objects.get("OriginalHand")
    if source is None or source.type != "MESH":
        raise SystemExit("no OriginalHand mesh in the current blend")

    kd = KDTree(len(source.data.vertices))
    for vertex in source.data.vertices:
        kd.insert(vertex.co, vertex.index)
    kd.balance()

    overlay, card_count = build_feathers(source)
    bind_indices, bind_weights = bind_vertices(
        [v.co.copy() for v in overlay.data.vertices], kd)
    stats = export_binary(args.out / "feather_overlay.bin", overlay,
                          bind_indices, bind_weights)
    print(f"feather cards: {card_count}")
    print(f"feather overlay: {stats}")

    args.out.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str((args.out / "original_arm_feathered.blend").resolve()))
    render(args.out, args.resolution, source)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
