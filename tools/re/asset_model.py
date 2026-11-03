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
VERTEX_ATTRIBUTES = [
    {"name": "position", "offset": 0, "components": 3},
    {"name": "normal", "offset": 12, "components": 3},
    {"name": "uv0", "offset": 24, "components": 2},
    {"name": "attribute3", "offset": 32, "components": 4},
    {"name": "tangent", "offset": 48, "components": 3},
    {"name": "bitangent", "offset": 60, "components": 3},
]


def _graph_tail(data: bytes, offset: int) -> dict:
    """Decode the bounded v3 name/value prefix; retain all uninterpreted bytes.

    Ghidra FUN_140205ec0 reads a u32 entry count, then a u8-length name,
    8 raw value bytes and two u16 fields for each entry. It subsequently reads
    a u32 reference count. The value fields' semantics are not established.
    """
    tail = data[offset:]
    if len(tail) < 4:
        return {"offset": offset, "size": len(tail), "raw_hex": tail.hex(), "parse_status": "truncated"}
    count = struct.unpack_from("<I", tail)[0]
    pos = 4
    entries = []
    if count > 100_000:
        return {"offset": offset, "size": len(tail), "entry_count": count,
                "raw_hex": tail.hex(), "parse_status": "implausible-count"}
    try:
        for _ in range(count):
            if pos >= len(tail):
                raise ValueError
            n = tail[pos]
            pos += 1
            if pos + n + 12 > len(tail):
                raise ValueError
            name_bytes = tail[pos:pos+n]
            pos += n
            raw_value = tail[pos:pos+8]
            field_a, field_b = struct.unpack_from("<HH", tail, pos + 8)
            pos += 12
            entries.append({"name": name_bytes.decode("utf-8", "replace"),
                            "name_raw_hex": name_bytes.hex(), "value_raw_hex": raw_value.hex(),
                            "field_u16_a": field_a, "field_u16_b": field_b})
        if pos + 4 > len(tail):
            raise ValueError
        reference_count = struct.unpack_from("<I", tail, pos)[0]
        count_offset = pos
        pos += 4
        return {"offset": offset, "size": len(tail), "entry_count": count,
                "entries": entries, "reference_count": reference_count,
                "reference_count_offset": offset + count_offset,
                "unparsed_hex": tail[pos:].hex(), "parse_status": "prefix-parsed"}
    except (ValueError, struct.error):
        return {"offset": offset, "size": len(tail), "entry_count": count,
                "entries": entries, "raw_hex": tail.hex(), "parse_status": "truncated-entry"}


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
    attribute_ranges = [[math.inf, -math.inf] for _ in range(18)]
    attribute_finite = [True] * 18
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
        for component in range(min(18, stride // 4)):
            value = struct.unpack_from("<f", data, offset + component * 4)[0]
            if not math.isfinite(value):
                attribute_finite[component] = False
                continue
            attribute_ranges[component][0] = min(attribute_ranges[component][0], value)
            attribute_ranges[component][1] = max(attribute_ranges[component][1], value)
        if stride >= 32 and not all(math.isfinite(v) for v in struct.unpack_from("<2f", data, offset + 24)):
            raise ValueError(f"non-finite UV at vertex {index}")
    min_index, max_index = vertex_count, 0
    for (value,) in struct.iter_unpack("<I", data[index_offset:arrays_end]):
        min_index = min(min_index, value)
        max_index = max(max_index, value)
        if value >= vertex_count:
            raise ValueError(f"index {value} exceeds vertex count {vertex_count}")
    if arrays_end != section_end:
        raise ValueError(f"mesh arrays end at {arrays_end}, declared section ends at {section_end}")

    attr_summary = []
    for attr in VERTEX_ATTRIBUTES:
        end = attr["offset"] + attr["components"] * 4
        if end <= stride:
            components = []
            for component in range(attr["components"]):
                i = attr["offset"] // 4 + component
                components.append(attribute_ranges[i] if attribute_finite[i] else None)
            attr_summary.append({**attr, "component_ranges": components,
                                 "all_finite": all(attribute_finite[attr["offset"] // 4:
                                     attr["offset"] // 4 + attr["components"]])})
    graph = _graph_tail(data, section_end)

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
        "vertex_attributes": attr_summary,
        "graph_tail": graph,
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


def write_blender_script(payload: pathlib.Path, output: pathlib.Path,
                         blend_output: pathlib.Path | None = None) -> None:
    """Write a Blender Python script that builds a mesh with all known attributes."""
    payload = payload.resolve()
    blend_output = (blend_output or payload.with_suffix(".blend")).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(f'''# Generated by tools/re/asset_model.py. Run with Blender --background --python.
import bpy, struct
from pathlib import Path
b = Path({str(payload)!r}).read_bytes()
section = 32
_, section_size = struct.unpack_from("<II", b, 24)
index_count, vertex_count = struct.unpack_from("<II", b, section + 4)
stride = b[section + 12]
v0 = section + 14
i0 = v0 + vertex_count * stride
verts = [struct.unpack_from("<3f", b, v0 + i*stride) for i in range(vertex_count)]
faces = [struct.unpack_from("<3I", b, i0 + i*4) for i in range(0, index_count, 3)]
mesh = bpy.data.meshes.new({payload.stem!r})
mesh.from_pydata(verts, [], faces)
mesh.update()
for name, offset, count in [("normal",12,3),("uv0",24,2),("attribute3",32,4),("tangent",48,3),("bitangent",60,3)]:
    if offset + count*4 > stride: continue
    if name == "uv0":
        layer = mesh.uv_layers.new(name=name)
        for poly in mesh.polygons:
            for li in poly.loop_indices:
                vi = mesh.loops[li].vertex_index
                layer.data[li].uv = struct.unpack_from("<2f", b, v0+vi*stride+offset)
        continue
    attr = mesh.attributes.new(name=name, type="FLOAT_VECTOR" if count == 3 else "FLOAT_COLOR", domain="POINT")
    for i in range(vertex_count):
        values = struct.unpack_from("<"+"f"*count, b, v0+i*stride+offset)
        if count == 3: attr.data[i].vector = values
        else: attr.data[i].color = values
obj = bpy.data.objects.new({payload.stem!r}, mesh)
bpy.context.collection.objects.link(obj)
bpy.ops.wm.save_as_mainfile(filepath={str(blend_output)!r})
''', encoding="utf-8")


def public_summary(model: dict) -> dict:
    return {key: value for key, value in model.items() if not key.startswith("_")}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("payload", type=pathlib.Path, help="decrypted tag-1 payload")
    parser.add_argument("--json", type=pathlib.Path, help="write inspection metadata")
    parser.add_argument("--obj", type=pathlib.Path, help="write Blender-compatible Wavefront OBJ")
    parser.add_argument("--blender-script", type=pathlib.Path,
                        help="write Blender Python script preserving all six vertex attribute groups")
    parser.add_argument("--blend-output", type=pathlib.Path,
                        help="output .blend path embedded in --blender-script (defaults beside payload)")
    args = parser.parse_args(argv)
    try:
        model = inspect_payload(args.payload.read_bytes())
        summary = public_summary(model)
        if args.json:
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(json.dumps(summary, indent=2) + "\n")
        if args.obj:
            write_obj(model, args.obj)
        if args.blender_script:
            write_blender_script(args.payload, args.blender_script, args.blend_output)
        print(json.dumps(summary, indent=2))
        return 0
    except (OSError, ValueError, struct.error) as error:
        print(f"asset-model: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
