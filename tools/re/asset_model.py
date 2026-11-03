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
EDITABLE_ATTRIBUTES = {item["name"]: item for item in VERTEX_ATTRIBUTES}


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


def rebuild_payload(data: bytes, edit: dict | None = None) -> bytes:
    """Rebuild a v3 model while preserving every byte outside supported fields.

    The current format has no independent canonical serializer for its graph tail.
    This therefore uses the validated source payload as a lossless template and
    patches only explicitly supplied vertex fields. This supports same-count edits.
    """
    model = inspect_payload(data)
    if model["graph_tail"].get("parse_status") != "prefix-parsed":
        raise ValueError("graph tail does not match the supported version-3 entry prefix")
    if edit is None:
        return data
    if not isinstance(edit, dict):
        raise ValueError("edit document must be a JSON object")
    if edit.get("vertex_count") != model["vertex_count"]:
        raise ValueError("edit vertex_count must exactly match the source model")
    attributes = edit.get("attributes")
    if not isinstance(attributes, dict) or not attributes:
        raise ValueError("edit document must contain a non-empty attributes object")
    unknown = set(attributes) - EDITABLE_ATTRIBUTES.keys()
    if unknown:
        raise ValueError(f"unsupported vertex attribute(s): {', '.join(sorted(unknown))}")
    stride = model["vertex_stride"]
    result = bytearray(data)
    for name, rows in attributes.items():
        spec = EDITABLE_ATTRIBUTES[name]
        offset = spec["offset"]
        components = spec["components"]
        if offset + components * 4 > stride:
            raise ValueError(f"attribute {name!r} is not present in the source stride")
        if not isinstance(rows, list) or len(rows) != model["vertex_count"]:
            raise ValueError(f"attribute {name!r} must have one row per source vertex")
        for index, row in enumerate(rows):
            if not isinstance(row, (list, tuple)) or len(row) != components:
                raise ValueError(f"attribute {name!r} vertex {index} must have {components} values")
            try:
                values = tuple(float(value) for value in row)
            except (TypeError, ValueError, OverflowError) as error:
                raise ValueError(f"attribute {name!r} vertex {index} is not numeric") from error
            if not all(math.isfinite(value) for value in values):
                raise ValueError(f"attribute {name!r} vertex {index} contains a non-finite value")
            try:
                struct.pack_into("<" + "f" * components,
                                 result, model["vertex_offset"] + index * stride + offset,
                                 *values)
            except (OverflowError, struct.error) as error:
                raise ValueError(f"attribute {name!r} vertex {index} exceeds float32 range") from error
    # Re-parse the rebuilt payload so bad normals/positions and structural edits
    # are caught before the output is written.
    inspect_payload(bytes(result))
    return bytes(result)


def write_blender_export_script(blend_input: pathlib.Path, edit_output: pathlib.Path,
                                script_output: pathlib.Path) -> None:
    """Write a Blender script that exports supported mesh values to edit JSON."""
    blend_input = blend_input.resolve()
    edit_output = edit_output.resolve()
    script_output.parent.mkdir(parents=True, exist_ok=True)
    script_output.write_text(f'''# Generated by tools/re/asset_model.py; run in Blender.
import bpy, json, math
from pathlib import Path
blend_path = Path({str(blend_input)!r})
edit_path = Path({str(edit_output)!r})
if bpy.data.filepath and Path(bpy.data.filepath).resolve() != blend_path:
    raise RuntimeError("loaded Blender file does not match --blend-input")
obj = bpy.context.view_layer.objects.active
if obj is None or obj.type != "MESH":
    candidates = [item for item in bpy.context.scene.objects if item.type == "MESH"]
    if len(candidates) == 1: obj = candidates[0]
if obj is None or obj.type != "MESH": raise RuntimeError("active object must be a mesh")
mesh = obj.data
count = len(mesh.vertices)
attrs = {{}}
attrs["position"] = [[float(c) for c in v.co] for v in mesh.vertices]
for name, components in [("normal",3),("attribute3",4),("tangent",3),("bitangent",3)]:
    if name not in mesh.attributes: raise RuntimeError("missing mesh attribute: " + name)
    attr = mesh.attributes[name]
    if attr.domain != "POINT" or len(attr.data) != count: raise RuntimeError("attribute is not point-domain: " + name)
    if components == 3:
        attrs[name] = [[float(x) for x in item.vector] for item in attr.data]
    else:
        attrs[name] = [[float(x) for x in item.color] for item in attr.data]
if "uv0" not in mesh.uv_layers: raise RuntimeError("missing uv0 layer")
uv = [[None, None] for _ in range(count)]
seen = [False] * count
for poly in mesh.polygons:
    for loop_index in poly.loop_indices:
        vertex_index = mesh.loops[loop_index].vertex_index
        value = [float(x) for x in mesh.uv_layers["uv0"].data[loop_index].uv]
        if seen[vertex_index] and any(abs(uv[vertex_index][k]-value[k]) > 1e-7 for k in range(2)):
            raise RuntimeError("UV seam requires vertex duplication; count-changing edits are unsupported")
        uv[vertex_index] = value
        seen[vertex_index] = True
if not all(seen): raise RuntimeError("some vertices have no uv0 corner")
attrs["uv0"] = uv
for name, rows in attrs.items():
    if any(not math.isfinite(x) for row in rows for x in row): raise RuntimeError("non-finite value in " + name)
edit_path.parent.mkdir(parents=True, exist_ok=True)
edit_path.write_text(json.dumps({{"vertex_count": count, "attributes": attrs}}, separators=(",", ":")) + "\\n")
print("wrote", edit_path, "with", count, "vertices")
''', encoding="utf-8")


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
obj.select_set(True)
bpy.context.view_layer.objects.active = obj
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
    parser.add_argument("--blender-export-script", type=pathlib.Path,
                        help="write Blender script to export supported mesh edits to JSON")
    parser.add_argument("--blend-input", type=pathlib.Path,
                        help=".blend file expected by --blender-export-script")
    parser.add_argument("--edit-json-output", type=pathlib.Path,
                        help="JSON destination embedded in --blender-export-script")
    parser.add_argument("--edit-json", type=pathlib.Path,
                        help="supported vertex attribute edits exported from Blender")
    parser.add_argument("--out", type=pathlib.Path,
                        help="write rebuilt payload (byte-identical without --edit-json)")
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
        if args.blender_export_script:
            if not args.blend_input or not args.edit_json_output:
                parser.error("--blender-export-script requires --blend-input and --edit-json-output")
            write_blender_export_script(args.blend_input, args.edit_json_output,
                                       args.blender_export_script)
        if args.edit_json and not args.out:
            parser.error("--edit-json requires --out")
        if args.out:
            if args.out.resolve() == args.payload.resolve():
                raise ValueError("output must not overwrite the source payload")
            edit = json.loads(args.edit_json.read_text(encoding="utf-8")) if args.edit_json else None
            source_payload = args.payload.read_bytes()
            rebuilt = rebuild_payload(source_payload, edit)
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_bytes(rebuilt)
            if rebuilt == source_payload:
                print(f"byte-identical rebuild: {args.out}", file=sys.stderr)
            else:
                print(f"rebuilt edited payload: {args.out}", file=sys.stderr)
        print(json.dumps(summary, indent=2))
        return 0
    except (OSError, ValueError, struct.error) as error:
        print(f"asset-model: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
