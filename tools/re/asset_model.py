#!/usr/bin/env python3
"""Inspect and export geometry sections found in HYPER DEMON tag-1 assets.

The parser follows FUN_140205ec0 / FUN_14021a710 (see docs/RE_PACK.md). It
preserves the decrypted payload and validates all declared lengths before
reading vertices or indices. OBJ is used for Blender-compatible geometry export.
"""
from __future__ import annotations

import argparse
import json
import math
import pathlib
import struct
import sys

GRAPH_OFFSET = 0x18
MESH_SECTION_OFFSET = GRAPH_OFFSET + 8
SECTION_HEADER_SIZE = 14  # u32 prefix, u32 index count, u32 vertex count, u8 stride, 1 pad byte
MAX_VERTICES = 10_000_000
MAX_INDICES = 60_000_000


def inspect_payload(data: bytes) -> dict:
    if len(data) < MESH_SECTION_OFFSET + SECTION_HEADER_SIZE:
        raise ValueError("payload is too short for a version-3 tag-1 mesh section")
    version, section_size = struct.unpack_from("<II", data, GRAPH_OFFSET)
    if version != 3:
        raise ValueError(f"unsupported tag-1 graph version {version}; expected 3")
    section_start = MESH_SECTION_OFFSET
    section_end = section_start + section_size
    if section_end < section_start or section_end > len(data):
        raise ValueError("declared binary section exceeds the payload")
    if section_size < SECTION_HEADER_SIZE:
        raise ValueError("binary section is shorter than its mesh header")

    index_count, vertex_count = struct.unpack_from("<II", data, section_start + 4)
    stride = data[section_start + 12]
    if not vertex_count or vertex_count > MAX_VERTICES:
        raise ValueError(f"invalid vertex count {vertex_count}")
    if not index_count or index_count > MAX_INDICES or index_count % 3:
        raise ValueError(f"invalid triangle index count {index_count}")
    if stride < 24 or stride > 4096:
        raise ValueError(f"unsupported vertex stride {stride}")

    vertex_offset = section_start + SECTION_HEADER_SIZE
    vertex_bytes = vertex_count * stride
    index_offset = vertex_offset + vertex_bytes
    index_bytes = index_count * 4
    arrays_end = index_offset + index_bytes
    if arrays_end < index_offset or arrays_end > section_end:
        raise ValueError("declared vertex and index arrays exceed the binary section")

    bounds = [[math.inf, -math.inf] for _ in range(3)]
    normal_length_range = [math.inf, 0.0]
    for index in range(vertex_count):
        offset = vertex_offset + index * stride
        values = struct.unpack_from("<6f", data, offset)
        if not all(math.isfinite(value) for value in values):
            raise ValueError(f"non-finite position or normal at vertex {index}")
        position, normal = values[:3], values[3:]
        for axis, value in enumerate(position):
            bounds[axis][0] = min(bounds[axis][0], value)
            bounds[axis][1] = max(bounds[axis][1], value)
        length = math.sqrt(sum(value * value for value in normal))
        normal_length_range[0] = min(normal_length_range[0], length)
        normal_length_range[1] = max(normal_length_range[1], length)
        uv = struct.unpack_from("<2f", data, offset + 24) if stride >= 32 else (0.0, 0.0)
        if not all(math.isfinite(value) for value in uv):
            raise ValueError(f"non-finite UV at vertex {index}")
    min_index, max_index = vertex_count, 0
    for (value,) in struct.iter_unpack("<I", data[index_offset:arrays_end]):
        min_index = min(min_index, value)
        max_index = max(max_index, value)
        if value >= vertex_count:
            raise ValueError(f"index {value} exceeds vertex count {vertex_count}")
    if section_end != arrays_end:
        raise ValueError(f"unexpected {section_end - arrays_end} bytes after mesh arrays")

    return {
        "format": "hyprdemon-tag1-mesh-section-v3",
        "graph_version": version,
        "payload_size": len(data),
        "mesh_section_offset": section_start,
        "mesh_section_size": section_size,
        "vertex_offset": vertex_offset,
        "vertex_count": vertex_count,
        "vertex_stride": stride,
        "index_offset": index_offset,
        "index_count": index_count,
        "index_width": 4,
        "triangle_count": index_count // 3,
        "bounds": {axis: bounds[i] for i, axis in enumerate("xyz")},
        "normal_length_range": normal_length_range,
        "index_range": [min_index, max_index],
        "trailing_payload_bytes": len(data) - section_end,
        "_data": data,
    }


def write_obj(model: dict, output: pathlib.Path) -> None:
    data = model["_data"]
    vertex_count = model["vertex_count"]
    stride = model["vertex_stride"]
    vertex_offset = model["vertex_offset"]
    index_count = model["index_count"]
    index_offset = model["index_offset"]
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="ascii", newline="\n") as stream:
        stream.write("# Exported from a locally extracted HYPER DEMON asset.\n")
        for index in range(vertex_count):
            position = struct.unpack_from("<3f", data, vertex_offset + index * stride)
            stream.write("v %.9g %.9g %.9g\n" % position)
        for index in range(vertex_count):
            uv = struct.unpack_from("<2f", data, vertex_offset + index * stride + 24)
            stream.write("vt %.9g %.9g\n" % uv)
        for index in range(vertex_count):
            normal = struct.unpack_from("<3f", data, vertex_offset + index * stride + 12)
            stream.write("vn %.9g %.9g %.9g\n" % normal)
        for start in range(0, index_count, 3):
            corners = []
            for index in range(start, start + 3):
                vertex = struct.unpack_from("<I", data, index_offset + index * 4)[0]
                one_based = vertex + 1
                corners.append(f"{one_based}/{one_based}/{one_based}")
            stream.write("f " + " ".join(corners) + "\n")


def public_summary(model: dict) -> dict:
    return {key: value for key, value in model.items() if not key.startswith("_")}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("payload", type=pathlib.Path, help="decrypted tag-1 payload")
    parser.add_argument("--json", type=pathlib.Path, help="write inspection metadata")
    parser.add_argument("--obj", type=pathlib.Path, help="write Blender-compatible Wavefront OBJ")
    args = parser.parse_args(argv)
    try:
        model = inspect_payload(args.payload.read_bytes())
        summary = public_summary(model)
        if args.json:
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(json.dumps(summary, indent=2) + "\n")
        if args.obj:
            write_obj(model, args.obj)
        print(json.dumps(summary, indent=2))
        return 0
    except (OSError, ValueError, struct.error) as error:
        print(f"asset-model: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
