#!/usr/bin/env python3
"""Import a live HYPER DEMON first-person hand VBO/IBO dump into Blender.

The hand draw was measured as 238 vertices at a 72-byte stride with six float
attributes (docs/RE_HAND.md):

    0   position   3
    12  normal     3
    24  uv0        2
    32  uv1        4   (x/y used, z/w zero in the captured pose)
    48  tangent    3
    60  bitangent  3

The captured buffer is already posed: there are no bone indices or weights, and
the four-float attribute is UV/mask data, not skinning. This importer therefore
reconstructs the visible mesh exactly as the game uploaded it, without inventing
a rig. It keeps both index halves as separate material slots because they cover
different surface groups even though they share vertices.

The dump is proprietary game geometry and must stay outside the repository.
The generated .blend and renders belong in the ignored assets/hand-extract/
directory; only this script is committed.

Usage:
    blender -b --python tools/assets/import_hand_vbo.py -- \
        --vbo /tmp/hand-vbo.bin --ibo /tmp/hand-ibo.bin \
        --out assets/hand-extract/original_arm.blend --render
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
from pathlib import Path

import bpy
from mathutils import Vector

STRIDE = 72
INDEX_HALF = 594
MATERIAL_NAMES = ("HandGroupA", "HandGroupB")


def parse_arguments() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--vbo", type=Path, required=True)
    parser.add_argument("--ibo", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--render", action="store_true")
    parser.add_argument("--resolution", type=int, default=960)
    return parser.parse_args(argv)


def read_dump(vbo_path: Path, ibo_path: Path):
    vbo = vbo_path.read_bytes()
    ibo = ibo_path.read_bytes()
    if len(vbo) % STRIDE:
        raise SystemExit(f"{vbo_path}: size {len(vbo)} is not a multiple of {STRIDE}")
    if len(ibo) % 4:
        raise SystemExit(f"{ibo_path}: size {len(ibo)} is not a multiple of 4")
    vertices = [struct.unpack_from("<18f", vbo, offset) for offset in range(0, len(vbo), STRIDE)]
    indices = list(struct.unpack(f"<{len(ibo) // 4}I", ibo))
    if max(indices) >= len(vertices):
        raise SystemExit("index buffer references a vertex outside the VBO")
    if len(indices) % 3:
        raise SystemExit("index count is not a multiple of three")
    return vertices, indices


def clear_scene() -> None:
    bpy.ops.wm.read_factory_settings(use_empty=True)


def make_material(name: str, colour: tuple[float, float, float], metallic: float,
                  roughness: float) -> bpy.types.Material:
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    bsdf = material.node_tree.nodes.get("Principled BSDF")
    if bsdf:
        bsdf.inputs["Base Color"].default_value = (*colour, 1.0)
        bsdf.inputs["Metallic"].default_value = metallic
        bsdf.inputs["Roughness"].default_value = roughness
        # A little coat keeps the low-poly facets readable under area lights.
        coat = bsdf.inputs.get("Coat Weight")
        if coat:
            coat.default_value = 0.25
    return material


def build_mesh(vertices, indices):
    mesh = bpy.data.meshes.new("OriginalHandMesh")
    positions = [tuple(vertex[0:3]) for vertex in vertices]
    faces = [tuple(indices[offset : offset + 3]) for offset in range(0, len(indices), 3)]
    mesh.from_pydata(positions, [], faces)
    mesh.update()

    uv0 = mesh.uv_layers.new(name="uv0")
    uv1 = mesh.uv_layers.new(name="uv1")
    for loop in mesh.loops:
        source = vertices[mesh.loops[loop.index].vertex_index]
        uv0.data[loop.index].uv = (source[6], source[7])
        uv1.data[loop.index].uv = (source[8], source[9])

    # Keep the captured per-vertex normals. Blender 4.1+ exposes the split
    # custom-normal API; older releases use the mesh-level flag.
    normals = [Vector(vertex[3:6]).normalized() for vertex in vertices]
    try:
        mesh.normals_split_custom_set_from_vertices(normals)
    except (AttributeError, RuntimeError):
        pass
    for polygon in mesh.polygons:
        polygon.use_smooth = True

    # The two 594-index runs are different surface groups with shared vertices,
    # not two hands. Assign them to separate materials so the structure is
    # visible in Blender and editable per group.
    material_a = make_material(MATERIAL_NAMES[0], (0.62, 0.42, 0.12), 0.55, 0.30)
    material_b = make_material(MATERIAL_NAMES[1], (0.10, 0.16, 0.34), 0.65, 0.26)
    mesh.materials.append(material_a)
    mesh.materials.append(material_b)
    half_faces = INDEX_HALF // 3
    for index, polygon in enumerate(mesh.polygons):
        polygon.material_index = 0 if index < half_faces else 1

    obj = bpy.data.objects.new("OriginalHand", mesh)
    bpy.context.collection.objects.link(obj)
    return obj


def look_at(obj, location: tuple[float, float, float],
             target: tuple[float, float, float]) -> None:
    direction = Vector(target) - Vector(location)
    obj.location = location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def add_camera(name: str, location, target, lens: float = 50.0) -> bpy.types.Object:
    camera_data = bpy.data.cameras.new(name)
    camera_data.lens = lens
    camera = bpy.data.objects.new(name, camera_data)
    bpy.context.collection.objects.link(camera)
    look_at(camera, location, target)
    return camera


def add_lights() -> None:
    for name, location, energy, size, colour in (
        ("Key", (0.7, -0.8, 0.9), 700.0, 1.2, (1.0, 0.92, 0.78)),
        ("Fill", (-0.8, -0.3, 0.35), 320.0, 1.5, (0.55, 0.72, 1.0)),
        ("Rim", (0.0, 0.65, 0.55), 500.0, 0.8, (0.95, 0.72, 1.0)),
    ):
        data = bpy.data.lights.new(name, "AREA")
        data.energy = energy
        data.shape = "DISK"
        data.size = size
        data.color = colour
        obj = bpy.data.objects.new(name, data)
        bpy.context.collection.objects.link(obj)
        look_at(obj, location, (0.0, 0.02, 0.02))


def configure_scene(resolution: int) -> None:
    scene = bpy.context.scene
    try:
        scene.render.engine = "BLENDER_EEVEE_NEXT"
    except TypeError:
        scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = resolution
    scene.render.resolution_y = int(resolution * 2 / 3)
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.film_transparent = False
    scene.world = bpy.data.worlds.new("HandWorld")
    scene.world.use_nodes = True
    background = scene.world.node_tree.nodes.get("Background")
    if background:
        background.inputs["Color"].default_value = (0.008, 0.010, 0.016, 1.0)
        background.inputs["Strength"].default_value = 0.35
    scene.view_settings.view_transform = "AgX"


def render_views(obj, out: Path, resolution: int) -> list[Path]:
    scene = bpy.context.scene
    scene.render.resolution_x = resolution
    scene.render.resolution_y = int(resolution * 2 / 3)
    out.parent.mkdir(parents=True, exist_ok=True)
    views = (
        ("original_arm_fp", (0.10, -0.74, 0.12), (0.00, 0.10, 0.035), 55.0),
        ("original_arm_side", (0.62, -0.32, 0.28), (0.00, 0.00, 0.02), 62.0),
        ("original_arm_top", (0.02, -0.18, 0.72), (0.00, 0.02, 0.00), 62.0),
    )
    written: list[Path] = []
    for name, location, target, lens in views:
        camera = add_camera(name, location, target, lens)
        scene.camera = camera
        path = out.parent / f"{name}.png"
        scene.render.filepath = str(path)
        bpy.ops.render.render(write_still=True)
        written.append(path)
    return written


def main() -> int:
    args = parse_arguments()
    vertices, indices = read_dump(args.vbo, args.ibo)
    clear_scene()
    obj = build_mesh(vertices, indices)
    add_lights()
    configure_scene(args.resolution)

    positions = [vertex[0:3] for vertex in vertices]
    bounds = [(min(point[axis] for point in positions),
               max(point[axis] for point in positions)) for axis in range(3)]
    print(f"hand mesh: {len(vertices)} vertices, {len(indices) // 3} triangles")
    print(f"bounds: {bounds}")
    print(f"groups: {INDEX_HALF // 3} + {len(indices) // 3 - INDEX_HALF // 3} triangles")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(args.out.resolve()))
    print(f"saved: {args.out}")

    if args.render:
        for path in render_views(obj, args.out, args.resolution):
            print(f"rendered: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
