#!/usr/bin/env python3
"""Bind the authored Berdly arm to the live HYPER DEMON hand buffer.

The authored mesh already carries a 72-byte game-style vertex record. This
tool maps it into the native hand's bind space, binds each authored vertex to
the four nearest original vertices with inverse-distance weights, and writes
the compact runtime overlay consumed by example.hand-hook.

The axis flip is deliberate. The native hand buffer's long axis points opposite
screen-up for this first-person pose; without the flip the wrist/feathers fill
the visible top and the authored claws sit below the frame. The bind indices
are computed before the flip so the overlay keeps the same live native delta
bindings as the verified capture.

Inputs are local extracts of the user's installation:
    assets/berdly-arm/hand_mesh.bin   (authored mesh, no game geometry)
    /tmp/hand-vbo.bin                 (238-vertex native bind pose)

Output (ignored by Git):
    assets/hand-extract/custom_overlay.bin

Binary layout, little-endian:
    0   magic        8 bytes  b"HYPRFEAT"
    8   version      u32      1
    12  vertex_count u32
    16  index_count  u32
    20  bind_count   u32      4
    24  reserved     2 x u32
    32  vertices     vertex_count * 64 bytes:
            position 3f, normal 3f, uv 2f,
            bind indices 4 x u32, bind weights 4f
        indices      index_count * u32

Usage:
    python3 tools/assets/build_custom_overlay.py
    python3 tools/assets/build_custom_overlay.py --no-flip-y
"""

from __future__ import annotations

import argparse
import heapq
import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MESH = ROOT / "assets/berdly-arm/hand_mesh.bin"
DEFAULT_BIND = Path("/tmp/hand-vbo.bin")
DEFAULT_OUT = ROOT / "assets/hand-extract/custom_overlay.bin"

MESH_MAGIC = b"HYPRMESH"
OVERLAY_MAGIC = b"HYPRFEAT"
OVERLAY_VERSION = 1
VERTEX_STRIDE = 72
BIND_COUNT = 4
BIND_EPSILON = 0.004

# Measured in the authored/native overlay reconstruction. The authored mesh is
# Z-long; the native hand is Y-long. The constants put the two bounding boxes in
# the same centre and account for the authored arm being narrower than the
# native shell.
AUTHORED_CENTER = (-0.040808, -0.058785, 0.109771)
NATIVE_CENTER = (-0.028100, -0.000150, -0.022200)
SCALE = (1.30, 1.00, 0.90)
OUTWARD_BIAS = 0.0060
CAMERA_OFFSET = (0.0, 0.0, 0.0250)


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--mesh", type=Path, default=DEFAULT_MESH)
    parser.add_argument("--bind", type=Path, default=DEFAULT_BIND)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--meta", type=Path, default=None)
    parser.add_argument("--no-flip-y", action="store_true",
                        help="keep the wrong screen axis for diagnostics")
    return parser.parse_args()


def normalize(vector: tuple[float, float, float]) -> tuple[float, float, float]:
    length = math.sqrt(sum(component * component for component in vector))
    if length <= 1e-12:
        return (0.0, 0.0, 1.0)
    return tuple(component / length for component in vector)


def read_mesh(path: Path):
    blob = path.read_bytes()
    if len(blob) < 48:
        raise SystemExit(f"{path}: truncated header")
    magic, version, vertex_count, index_count, stride = struct.unpack_from("<8sIIII", blob, 0)
    if magic != MESH_MAGIC or version != 1 or stride != VERTEX_STRIDE:
        raise SystemExit(f"{path}: unexpected mesh header")
    expected = 48 + vertex_count * VERTEX_STRIDE + index_count * 4
    if len(blob) != expected:
        raise SystemExit(f"{path}: size {len(blob)} != {expected}")
    vertices = [
        struct.unpack_from("<18f", blob, 48 + index * VERTEX_STRIDE)
        for index in range(vertex_count)
    ]
    indices = list(struct.unpack_from(f"<{index_count}I", blob, 48 + vertex_count * VERTEX_STRIDE))
    if any(index >= vertex_count for index in indices):
        raise SystemExit(f"{path}: index out of range")
    return vertices, indices


def read_bind(path: Path):
    blob = path.read_bytes()
    if len(blob) % VERTEX_STRIDE:
        raise SystemExit(f"{path}: size is not a multiple of {VERTEX_STRIDE}")
    return [struct.unpack_from("<3f", blob, offset) for offset in range(0, len(blob), VERTEX_STRIDE)]


def transform_vertex(values) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
    x, y, z = values[0:3]
    nx, ny, nz = values[3:6]
    normal = normalize((nx / SCALE[0], nz / SCALE[1], -ny / SCALE[2]))
    position = (
        (x - AUTHORED_CENTER[0]) * SCALE[0] + NATIVE_CENTER[0] +
        normal[0] * OUTWARD_BIAS + CAMERA_OFFSET[0],
        (z - AUTHORED_CENTER[2]) * SCALE[1] + NATIVE_CENTER[1] +
        normal[1] * OUTWARD_BIAS + CAMERA_OFFSET[1],
        -(y - AUTHORED_CENTER[1]) * SCALE[2] + NATIVE_CENTER[2] +
        normal[2] * OUTWARD_BIAS + CAMERA_OFFSET[2],
    )
    return position, normal


def nearest_bind(point, bind_positions):
    nearest = heapq.nsmallest(
        BIND_COUNT,
        ((sum((point[axis] - candidate[axis]) ** 2 for axis in range(3)), index)
         for index, candidate in enumerate(bind_positions)),
        key=lambda value: value[0],
    )
    raw = [(index, 1.0 / (math.sqrt(distance) + BIND_EPSILON)) for distance, index in nearest]
    total = sum(weight for _index, weight in raw)
    return [index for index, _weight in raw], [weight / total for _index, weight in raw]


def main() -> int:
    args = arguments()
    vertices, indices = read_mesh(args.mesh)
    bind_positions = read_bind(args.bind)

    positions: list[tuple[float, float, float]] = []
    normals: list[tuple[float, float, float]] = []
    uvs: list[tuple[float, float]] = []
    bind_indices: list[list[int]] = []
    bind_weights: list[list[float]] = []
    for values in vertices:
        position, normal = transform_vertex(values)
        indices4, weights4 = nearest_bind(position, bind_positions)
        positions.append(position)
        normals.append(normal)
        uvs.append((values[6], values[7]))
        bind_indices.append(indices4)
        bind_weights.append(weights4)

    if not args.no_flip_y:
        # The native hand's long axis is opposite screen-up. Flip after binding;
        # the verified capture uses the pre-flip nearest-native assignments.
        positions = [(x, -y, z) for x, y, z in positions]
        normals = [normalize((x, -y, z)) for x, y, z in normals]

    data = bytearray(struct.pack("<8sIIIIII", OVERLAY_MAGIC, OVERLAY_VERSION,
                                 len(vertices), len(indices), BIND_COUNT, 0, 0))
    max_weight_error = 0.0
    used = set()
    for position, normal, uv, indices4, weights4 in zip(
            positions, normals, uvs, bind_indices, bind_weights):
        max_weight_error = max(max_weight_error, abs(sum(weights4) - 1.0))
        used.update(indices4)
        data.extend(struct.pack("<3f3f2f4I4f", *position, *normal, *uv,
                                *indices4, *weights4))
    data.extend(struct.pack(f"<{len(indices)}I", *indices))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(data)

    bbox_min = [min(position[axis] for position in positions) for axis in range(3)]
    bbox_max = [max(position[axis] for position in positions) for axis in range(3)]
    metadata = {
        "schema": "hyprlodr.custom-overlay/1",
        "source_mesh": str(args.mesh.relative_to(ROOT)) if args.mesh.is_relative_to(ROOT) else str(args.mesh),
        "bind_source": str(args.bind),
        "vertex_count": len(vertices),
        "index_count": len(indices),
        "triangle_count": len(indices) // 3,
        "bind_count": BIND_COUNT,
        "flipped_y": not args.no_flip_y,
        "bbox_min": [round(value, 6) for value in bbox_min],
        "bbox_max": [round(value, 6) for value in bbox_max],
        "bytes": len(data),
        "used_native_vertices": len(used),
        "max_bind_weight_error": max_weight_error,
    }
    if args.meta:
        args.meta.write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"custom overlay: {len(vertices)} vertices, {len(indices)} indices, {len(data)} bytes")
    print(f"bbox {tuple(bbox_min)} .. {tuple(bbox_max)}")
    print(f"used native vertices: {len(used)}; max bind error: {max_weight_error:.3e}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
