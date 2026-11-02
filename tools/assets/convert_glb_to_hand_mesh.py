#!/usr/bin/env python3
"""Convert the baked runtime16 glTF arm into the game's 72-byte vertex layout.

The output is a self-contained little-endian binary:

    magic        8 bytes  b"HYPRMESH"
    version      u32      1
    vertex_count u32
    index_count  u32
    stride       u32      72
    bbox_min     3 x f32  (after the hand-space transform)
    bbox_max     3 x f32
    vertices     vertex_count * 72 bytes
    indices      index_count * u32

Each vertex is 18 floats, matching the layout measured from the live hand draw:

    0   position   3
    12  normal     3
    24  uv0        2
    32  uv1        4 (two unused floats, matching the game's stride)
    48  tangent    3
    60  bitangent  3

The GLB is the validated 16-bone runtime arm, so the source geometry is our own
authored asset; no game geometry is read or embedded. Tangents are a stable
placeholder; the baked rig and actions are not used by this first static upload.

The glTF exporter keeps the authored mesh on a Z-long axis with the claws at
-Z. In the live hand pass +Z is screen-up, so the raw mesh renders vertically
with the claws at the near/bottom end. The default 180 degree X remap
(x, y, z) -> (x, -y, -z) flips that long axis, putting the claws at the far/top
end and the wrist at the bottom, matching the stock arm's direction. A 90
degree remap was tested live and laid the arm horizontally, so it is not the
correct quarter-turn. Normals are rotated by the same matrix.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_GLB = ROOT / "assets/berdly-arm/berdly_arm.runtime16.glb"
DEFAULT_OUT = ROOT / "assets/berdly-arm/hand_mesh.bin"
DEFAULT_META = ROOT / "assets/berdly-arm/hand_mesh.meta.json"

MAGIC = b"HYPRMESH"
VERSION = 1
STRIDE = 72
COMPONENTS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
FORMATS = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}
SIZES = {5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4}


def read_glb(path: Path) -> tuple[dict, bytes]:
    blob = path.read_bytes()
    magic, version, length = struct.unpack_from("<4sII", blob, 0)
    if magic != b"glTF" or version != 2 or length != len(blob):
        raise SystemExit(f"{path}: not a glTF 2.0 binary")
    offset = 12
    doc = None
    binary = b""
    while offset < len(blob):
        chunk_length, chunk_type = struct.unpack_from("<II", blob, offset)
        payload = blob[offset + 8 : offset + 8 + chunk_length]
        if chunk_type == 0x4E4F534A:
            doc = json.loads(payload)
        elif chunk_type == 0x004E4942:
            binary = payload
        offset += 8 + chunk_length
    if doc is None:
        raise SystemExit(f"{path}: missing JSON chunk")
    return doc, binary


class Accessors:
    def __init__(self, doc: dict, binary: bytes):
        self.doc = doc
        self.binary = binary

    def read(self, index: int) -> list[tuple]:
        accessor = self.doc["accessors"][index]
        view = self.doc["bufferViews"][accessor["bufferView"]]
        count = accessor["count"]
        width = COMPONENTS[accessor["type"]]
        fmt = FORMATS[accessor["componentType"]]
        size = SIZES[accessor["componentType"]]
        stride = view.get("byteStride") or width * size
        base = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        chunk = "<" + fmt * width
        return [
            struct.unpack_from(chunk, self.binary, base + i * stride)
            for i in range(count)
        ]


def rotate_x(vector: tuple[float, ...], degrees: float) -> tuple[float, ...]:
    """Rotate a 3-vector about +X; other components pass through unchanged."""
    radians = math.radians(degrees)
    cosine, sine = math.cos(radians), math.sin(radians)
    x, y, z = vector[:3]
    return (x, y * cosine - z * sine, y * sine + z * cosine)


def convert(glb: Path, out: Path, meta: Path, scale: float, offset: tuple[float, float, float],
            rotate_x_degrees: float = 180.0) -> None:
    doc, binary = read_glb(glb)
    accessors = Accessors(doc, binary)
    if len(doc.get("meshes", [])) != 1:
        raise SystemExit(f"{glb}: expected one mesh, found {len(doc.get('meshes', []))}")

    vertices: list[bytes] = []
    indices: list[int] = []
    material_slots: list[int] = []
    for primitive in doc["meshes"][0]["primitives"]:
        attrs = primitive["attributes"]
        if "POSITION" not in attrs or "NORMAL" not in attrs or "TEXCOORD_0" not in attrs:
            raise SystemExit(f"{glb}: primitive lacks POSITION/NORMAL/TEXCOORD_0")
        positions = accessors.read(attrs["POSITION"])
        normals = accessors.read(attrs["NORMAL"])
        uvs = accessors.read(attrs["TEXCOORD_0"])
        if not (len(positions) == len(normals) == len(uvs)):
            raise SystemExit("primitive attribute counts differ")
        material_slot = int(primitive.get("material", 0))
        if not 0 <= material_slot < 5:
            raise SystemExit(f"unsupported runtime material slot {material_slot}; expected 0..4")
        material_slots.append(material_slot)
        base = len(vertices)
        for position, normal, uv in zip(positions, normals, uvs):
            position = rotate_x(position, rotate_x_degrees)
            normal = rotate_x(normal, rotate_x_degrees)
            record = struct.pack(
                "<18f",
                position[0] * scale + offset[0],
                position[1] * scale + offset[1],
                position[2] * scale + offset[2],
                normal[0],
                normal[1],
                normal[2],
                # Keep authored UVs: the replacement binds its own opaque blue
                # feather texture to tex_diffuse, while the game's other
                # material samplers remain untouched for its gold reflections.
                # Five diffuse atlas columns preserve the authored material
                # identity that a single game hand shader would otherwise lose.
                # Keep a small gutter at each edge to prevent linear-filter bleed.
                (material_slot + 0.035 + uv[0] * 0.93) / 5.0,
                uv[1],
                # uv1 from the live hand vertex; the cube proved this mask is
                # opaque, while zeroed uv1 sampled an empty region.
                0.023678,
                0.092198,
                0.0,
                0.0,
                1.0,
                0.0,
                0.0,
                0.0,
                1.0,
                0.0,
            )
            if len(record) != STRIDE:
                raise AssertionError(len(record))
            vertices.append(record)
        if "indices" not in primitive:
            raise SystemExit("primitive lacks indices")
        primitive_indices = [value[0] for value in accessors.read(primitive["indices"])]
        if max(primitive_indices) >= len(positions):
            raise SystemExit("index out of range")
        indices.extend(base + value for value in primitive_indices)

    if not vertices or not indices:
        raise SystemExit("empty mesh")
    if len(indices) % 3:
        raise SystemExit("index count is not a multiple of three")

    positions = [struct.unpack_from("<3f", record, 0) for record in vertices]
    bbox_min = [min(p[axis] for p in positions) for axis in range(3)]
    bbox_max = [max(p[axis] for p in positions) for axis in range(3)]

    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("wb") as handle:
        handle.write(struct.pack("<8sIIII6f", MAGIC, VERSION, len(vertices), len(indices), STRIDE,
                                 *bbox_min, *bbox_max))
        handle.write(b"".join(vertices))
        handle.write(struct.pack(f"<{len(indices)}I", *indices))

    metadata = {
        "schema": "hyprlodr.hand-mesh/1",
        "source": str(glb.relative_to(ROOT)),
        "output": str(out.relative_to(ROOT)),
        "vertex_count": len(vertices),
        "index_count": len(indices),
        "triangle_count": len(indices) // 3,
        "stride": STRIDE,
        "scale": scale,
        "offset": list(offset),
        "rotate_x_degrees": rotate_x_degrees,
        "bbox_min": [round(value, 6) for value in bbox_min],
        "bbox_max": [round(value, 6) for value in bbox_max],
        "bytes": out.stat().st_size,
        "material_atlas": {
            "columns": 5,
            "slots": sorted(set(material_slots)),
            "source_materials": [material.get("name", f"material_{index}")
                                 for index, material in enumerate(doc.get("materials", []))],
        },
        "note": "Static bind-pose geometry in game hand space. Authored UVs address five blue/gold diffuse atlas regions; original game reflection and gradient samplers stay bound.",
    }
    meta.write_text(json.dumps(metadata, indent=2) + "\n")
    print(
        f"{out}: {len(vertices)} verts, {len(indices)} indices, "
        f"{len(indices) // 3} tris, {out.stat().st_size} bytes, "
        f"bbox {bbox_min} .. {bbox_max}"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--glb", type=Path, default=DEFAULT_GLB)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--meta", type=Path, default=DEFAULT_META)
    parser.add_argument("--scale", type=float, default=0.32)
    parser.add_argument("--offset", type=float, nargs=3,
                        default=(-0.0440, -0.1442, -0.1000),
                        metavar=("X", "Y", "Z"))
    parser.add_argument("--rotate-x", type=float, default=180.0, metavar="DEGREES",
                        help="flip the exported -Z long axis into screen-up +Z (default 180)")
    args = parser.parse_args()
    convert(args.glb, args.out, args.meta, args.scale, tuple(args.offset), args.rotate_x)


if __name__ == "__main__":
    main()
