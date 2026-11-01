#!/usr/bin/env python3
"""Build a same-format test mesh from a locally dumped HYPER DEMON hand VBO.

The dump is game data and must stay outside the repository. This tool only prints
layout facts and writes derived test data. It is research tooling for the live
draw-substitution experiment, not a runtime mod.

Usage:
    python3 tools/re/hand_vbo_test.py --vbo /tmp/hand-vbo.bin \
        --ibo /tmp/hand-ibo.bin --out /tmp/hand-test

The hand draw measured in docs/RE_HAND.md uses a 72-byte stride with six float
attributes: 3, 3, 2, 4, 3, 3. The first three floats are position and the next
three are normal. The helper copies one existing 72-byte record (including its
bone/UV data), replaces position and normal, and emits eight vertices plus 36
unsigned-short indices for a cube.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
from pathlib import Path

STRIDE = 72
POSITION_OFFSET = 0
NORMAL_OFFSET = 12
CUBE_SIZE_DEFAULT = 0.08


def read_vbo(path: Path) -> tuple[bytes, list[bytes], list[tuple[float, float, float]]]:
    data = path.read_bytes()
    if not data:
        raise ValueError("VBO dump is empty")
    if len(data) % STRIDE != 0:
        raise ValueError(f"VBO size {len(data)} is not a multiple of {STRIDE}")
    records = [data[i : i + STRIDE] for i in range(0, len(data), STRIDE)]
    positions = [struct.unpack_from("<3f", record, POSITION_OFFSET) for record in records]
    return data, records, positions


def cube_vertices(anchor: tuple[float, float, float], size: float) -> list[tuple[float, float, float]]:
    points: list[tuple[float, float, float]] = []
    for dz in (-1.0, 1.0):
        for dy in (-1.0, 1.0):
            for dx in (-1.0, 1.0):
                points.append(
                    (
                        anchor[0] + dx * size,
                        anchor[1] + dy * size,
                        anchor[2] + dz * size,
                    )
                )
    return points


def cube_indices() -> list[int]:
    # Corners are ordered x fastest, then y, then z.
    quads = (
        (0, 1, 3, 2),
        (4, 6, 7, 5),
        (0, 4, 5, 1),
        (2, 3, 7, 6),
        (0, 2, 6, 4),
        (1, 5, 7, 3),
    )
    indices: list[int] = []
    for a, b, c, d in quads:
        indices.extend((a, b, c, a, c, d))
    return indices


def build(vbo_path: Path, vertex_index: int, size: float) -> tuple[bytes, bytes, dict]:
    raw, records, positions = read_vbo(vbo_path)
    if vertex_index < 0 or vertex_index >= len(records):
        raise ValueError(f"vertex index {vertex_index} is outside 0..{len(records) - 1}")
    if not math.isfinite(size) or size <= 0.0:
        raise ValueError("--size must be a positive finite number")

    anchor = positions[vertex_index]
    base = records[vertex_index]
    vertices = bytearray()
    for point in cube_vertices(anchor, size):
        record = bytearray(base)
        struct.pack_into("<3f", record, POSITION_OFFSET, *point)
        normal = [point[i] - anchor[i] for i in range(3)]
        length = math.sqrt(sum(value * value for value in normal))
        struct.pack_into("<3f", record, NORMAL_OFFSET, *(value / length for value in normal))
        vertices.extend(record)

    indices = cube_indices()
    index_bytes = struct.pack(f"<{len(indices)}H", *indices)
    bounds = {
        axis: [min(point[i] for point in positions), max(point[i] for point in positions)]
        for i, axis in enumerate("xyz")
    }
    layout = {
        "source": str(vbo_path),
        "bytes": len(raw),
        "vertices": len(records),
        "stride": STRIDE,
        "position_offset": POSITION_OFFSET,
        "normal_offset": NORMAL_OFFSET,
        "bounds": bounds,
        "anchor_vertex": vertex_index,
        "anchor_position": list(anchor),
        "cube_size": size,
        "output_vertices": 8,
        "output_indices": len(indices),
    }
    return bytes(vertices), index_bytes, layout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vbo", required=True, type=Path, help="local hand VBO dump")
    parser.add_argument("--ibo", type=Path, help="optional local hand IBO dump")
    parser.add_argument("--out", required=True, type=Path, help="directory for derived test data")
    parser.add_argument("--vertex", type=int, default=0, help="record to copy attributes and weights from")
    parser.add_argument("--size", type=float, default=CUBE_SIZE_DEFAULT, help="half-size of the test cube")
    args = parser.parse_args()

    try:
        vertices, indices, layout = build(args.vbo, args.vertex, args.size)
    except (OSError, ValueError) as error:
        parser.error(str(error))

    args.out.mkdir(parents=True, exist_ok=True)
    vbo_out = args.out / "test-cube.vbo"
    ibo_out = args.out / "test-cube.ibo"
    vbo_out.write_bytes(vertices)
    ibo_out.write_bytes(indices)

    if args.ibo:
        try:
            index_data = args.ibo.read_bytes()
            layout["source_indices"] = len(index_data) // 2
            layout["source_index_bytes"] = len(index_data)
        except OSError as error:
            parser.error(str(error))

    (args.out / "layout.json").write_text(json.dumps(layout, indent=2) + "\n")

    print(f"source: {layout['vertices']} vertices, {layout['bytes']} bytes, stride {STRIDE}")
    if "source_indices" in layout:
        print(f"source indices: {layout['source_indices']} (unsigned short)")
    print(f"anchor vertex {args.vertex} at {tuple(round(v, 4) for v in layout['anchor_position'])}")
    print(f"bounds: {layout['bounds']}")
    print(f"wrote: {vbo_out} ({len(vertices)} bytes) and {ibo_out} ({len(indices)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
