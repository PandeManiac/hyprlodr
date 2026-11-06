#!/usr/bin/env python3
"""Inspect and compare a passive HYPRLODR first-person hand GPU capture.

The capture is a fixed little-endian header followed by the exact VBO bytes and
index bytes read from OpenGL in the original draw shim. This tool keeps source
payloads local and reports exact equality, topology/attribute correspondence,
and position-fit measurements independently.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import struct
import sys

try:
    from .asset_model import inspect_payload
except ImportError:  # direct invocation as `python tools/re/hand_capture.py`
    from asset_model import inspect_payload

MAGIC = b"HYPRCAP1"
HEADER = struct.Struct("<8s12I2Q8I")
HAND_VERTICES = 238
HAND_STRIDE = 72
HAND_INDICES = 1188
ENLARGED_HAND_VERTICES = 239
ENLARGED_HAND_INDICES = 1191
SUPPORTED_SIGNATURES = {(HAND_VERTICES, HAND_INDICES),
                        (ENLARGED_HAND_VERTICES, ENLARGED_HAND_INDICES)}
GL_UNSIGNED_INT = 0x1405
ATTRIBUTE_OFFSETS = (0, 12, 24, 32, 48, 60)
ATTRIBUTE_WIDTHS = (12, 12, 8, 16, 12, 12)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_capture(data: bytes) -> dict:
    if len(data) < HEADER.size:
        raise ValueError("capture is shorter than its fixed header")
    fields = HEADER.unpack_from(data)
    (magic, version, header_size, draw_ordinal, frame_id, mode, draw_count,
     index_type, vertex_count, stride, vertex_bytes, index_count, index_width,
     index_offset, timestamp_ms, array_buffer, element_buffer, *attribute_offsets) = fields
    if magic != MAGIC:
        raise ValueError("capture magic does not match HYPRCAP1")
    if version != 1 or header_size != HEADER.size:
        raise ValueError("unsupported capture version or header size")
    if (mode != 7 or (vertex_count, index_count) not in SUPPORTED_SIGNATURES or
            draw_count != index_count or index_type != GL_UNSIGNED_INT or
            stride != HAND_STRIDE or vertex_bytes != vertex_count * HAND_STRIDE or
            index_width != 4):
        raise ValueError("capture draw layout differs from the verified hand signature")
    if draw_ordinal == 0 or array_buffer == 0 or element_buffer == 0:
        raise ValueError("capture is missing its draw ordinal or live buffer identifiers")
    if tuple(attribute_offsets) != ATTRIBUTE_OFFSETS:
        raise ValueError("capture attribute offsets differ from the verified 72-byte layout")
    if index_offset % index_width:
        raise ValueError("capture index offset is not aligned to its element width")
    expected_size = HEADER.size + vertex_bytes + index_count * index_width
    if len(data) != expected_size:
        raise ValueError(f"capture size {len(data)} does not equal expected {expected_size}")
    vertex_data = data[HEADER.size:HEADER.size + vertex_bytes]
    index_data = data[HEADER.size + vertex_bytes:]
    indices = struct.unpack(f"<{index_count}I", index_data)
    if any(index >= vertex_count for index in indices):
        raise ValueError("captured index addresses a vertex outside the captured VBO")
    return {
        "format": "hyprlodr-passive-hand-gl-capture-v1",
        "draw_ordinal": draw_ordinal,
        "frame_id_pair_estimate": frame_id,
        "frame_id_note": "derived from two measured adjacent hand passes, not a swap callback",
        "timestamp_ms_since_boot": timestamp_ms,
        "draw_mode": "GL_TRIANGLES",
        "draw_count": draw_count,
        "index_type": "GL_UNSIGNED_INT",
        "index_offset_bytes": index_offset,
        "vertex_count": vertex_count,
        "vertex_stride": stride,
        "vertex_bytes": vertex_bytes,
        "index_count": index_count,
        "array_buffer_id": array_buffer,
        "element_buffer_id": element_buffer,
        "attribute_offsets": list(attribute_offsets),
        "vertex_sha256": sha256(vertex_data),
        "index_sha256": sha256(index_data),
        "vertex_data": vertex_data,
        "index_data": index_data,
    }


def _solve(matrix: list[list[float]], vector: list[float]) -> list[float]:
    """Solve a small dense linear system with pivoted Gauss-Jordan elimination."""
    n = len(vector)
    augmented = [list(matrix[row]) + [vector[row]] for row in range(n)]
    for col in range(n):
        pivot = max(range(col, n), key=lambda row: abs(augmented[row][col]))
        if abs(augmented[pivot][col]) < 1e-12:
            raise ValueError("position correspondence is rank-deficient")
        augmented[col], augmented[pivot] = augmented[pivot], augmented[col]
        scale = augmented[col][col]
        augmented[col] = [value / scale for value in augmented[col]]
        for row in range(n):
            if row == col:
                continue
            factor = augmented[row][col]
            augmented[row] = [a - factor * b for a, b in zip(augmented[row], augmented[col])]
    return [augmented[row][-1] for row in range(n)]


def _fit_affine(source: list[tuple[float, float, float]],
                target: list[tuple[float, float, float]]) -> dict | None:
    if len(source) != len(target) or len(source) < 4:
        return None
    design = [(x, y, z, 1.0) for x, y, z in source]
    gram = [[sum(row[i] * row[j] for row in design) for j in range(4)] for i in range(4)]
    coefficients = []
    try:
        for axis in range(3):
            rhs = [sum(row[i] * point[axis] for row, point in zip(design, target))
                   for i in range(4)]
            coefficients.append(_solve(gram, rhs))
    except ValueError:
        return None
    errors = []
    for row, actual in zip(design, target):
        predicted = [sum(row[i] * coefficients[axis][i] for i in range(4))
                     for axis in range(3)]
        errors.append(math.sqrt(sum((predicted[i] - actual[i]) ** 2 for i in range(3))))
    errors.sort()
    bounds = [[min(point[i] for point in target), max(point[i] for point in target)]
              for i in range(3)]
    diagonal = math.sqrt(sum((pair[1] - pair[0]) ** 2 for pair in bounds))
    rmse = math.sqrt(sum(error * error for error in errors) / len(errors))
    return {
        "affine_coefficients_by_output_axis": coefficients,
        "rmse": rmse,
        "p95": errors[min(len(errors) - 1, math.ceil(len(errors) * 0.95) - 1)],
        "max_error": errors[-1],
        "target_bounds_diagonal": diagonal,
        "normalized_rmse": rmse / diagonal if diagonal else None,
        "interpretation": "global affine fit only; non-rigid CPU posing may produce larger residuals",
    }


def _normal_affine_fit(source_vertices: bytes, target_vertices: bytes,
                       vertex_count: int, stride: int,
                       mapping: list[int], position_fit: dict | None) -> dict | None:
    if not position_fit:
        return None
    c = position_fit["affine_coefficients_by_output_axis"]
    matrix = [[c[row][col] for col in range(3)] for row in range(3)]
    a, b, d = matrix[0]
    e, f, g = matrix[1]
    h, i, j = matrix[2]
    det = a * (f * j - g * i) - b * (e * j - g * h) + d * (e * i - f * h)
    if abs(det) < 1e-12:
        return None
    inverse = [
        [(f*j-g*i)/det, (d*i-b*j)/det, (b*g-d*f)/det],
        [(g*h-e*j)/det, (a*j-d*h)/det, (d*e-a*g)/det],
        [(e*i-f*h)/det, (b*h-a*i)/det, (a*f-b*e)/det],
    ]
    angles = []
    for source_index, target_index in enumerate(mapping):
        normal = struct.unpack_from("<3f", source_vertices, source_index * stride + 12)
        actual = struct.unpack_from("<3f", target_vertices, target_index * stride + 12)
        # Normals transform by the inverse transpose of the fitted position map.
        predicted = [sum(inverse[col][row] * normal[col] for col in range(3))
                     for row in range(3)]
        predicted_length = math.sqrt(sum(value * value for value in predicted))
        actual_length = math.sqrt(sum(value * value for value in actual))
        if predicted_length == 0.0 or actual_length == 0.0:
            return None
        cosine = sum(predicted[k] * actual[k] for k in range(3)) / (predicted_length * actual_length)
        angles.append(math.degrees(math.acos(max(-1.0, min(1.0, cosine)))) )
    angles.sort()
    return {
        "mean_angular_error_degrees": sum(angles) / len(angles),
        "p95_angular_error_degrees": angles[min(len(angles) - 1,
                                                  math.ceil(len(angles) * 0.95) - 1)],
        "max_angular_error_degrees": angles[-1],
        "interpretation": "normal directions compared after inverse-transpose of the global position fit",
    }


def _model_arrays(payload: bytes) -> dict:
    model = inspect_payload(payload)
    stride = model["vertex_stride"]
    start = model["vertex_offset"]
    vertex_data = payload[start:start + model["vertex_count"] * stride]
    index_start = model["index_offset"]
    index_data = payload[index_start:index_start + model["index_count"] * 4]
    return {"model": model, "vertex_data": vertex_data, "index_data": index_data,
            "payload": payload}


def _positions(data: bytes, count: int, stride: int, offset: int = 0):
    return [struct.unpack_from("<3f", data, offset + i * stride) for i in range(count)]


def compare_source(capture: dict, payload: bytes, label: str) -> dict:
    source = _model_arrays(payload)
    model = source["model"]
    captured_vertices = capture["vertex_data"]
    captured_indices = capture["index_data"]
    source_vertices = source["vertex_data"]
    source_indices = source["index_data"]
    source_vertices_count = model["vertex_count"]
    source_indices_count = model["index_count"]
    capture_vertices_count = capture["vertex_count"]
    capture_indices_count = capture["index_count"]
    topology_dimensions_match = (source_vertices_count == capture_vertices_count and
                                 source_indices_count == capture_indices_count and
                                 model["vertex_stride"] == capture["vertex_stride"])
    extra_vertex_referenced = (capture_vertices_count > HAND_VERTICES and any(
        index == HAND_VERTICES for (index,) in struct.iter_unpack("<I", captured_indices)))
    additional_triangle_present = (capture_indices_count >= ENLARGED_HAND_INDICES and
                                   capture_indices_count % 3 == 0)
    extra_triangle_bytes = 3 * 4
    extra_triangle_matches_source = (
        source_indices_count == ENLARGED_HAND_INDICES and
        capture_indices_count == ENLARGED_HAND_INDICES and
        source_indices[-extra_triangle_bytes:] == captured_indices[-extra_triangle_bytes:])
    common = {
        "label": label,
        "source_vertex_count": source_vertices_count,
        "source_index_count": source_indices_count,
        "captured_vertex_count": capture_vertices_count,
        "captured_index_count": capture_indices_count,
        "topology_matches": False,
        "topology_dimensions_match": topology_dimensions_match,
        "index_bytes_match_exactly": source_indices == captured_indices,
        "vertex_bytes_match_exactly": source_vertices == captured_vertices,
        "source_vertex_sha256": sha256(source_vertices),
        "captured_vertex_sha256": capture["vertex_sha256"],
        "source_index_sha256": sha256(source_indices),
        "captured_index_sha256": capture["index_sha256"],
        "extra_vertex_present": capture_vertices_count > HAND_VERTICES,
        "extra_vertex_referenced": extra_vertex_referenced,
        "additional_triangle_indices_present": additional_triangle_present,
        "additional_triangle_indices_match_source": extra_triangle_matches_source,
    }
    if not topology_dimensions_match:
        return common | {
            "assessment": "unsupported-topology-comparison",
            "topology_correspondence": False,
            "notes": ["byte and transform comparisons require equal vertex and index counts"],
        }
    vertex_count, stride = model["vertex_count"], model["vertex_stride"]
    exact_vertex_bytes = captured_vertices == source_vertices
    exact_index_bytes = captured_indices == source_indices

    def signatures(vertices: bytes):
        return [vertices[i * stride + 24:i * stride + 48] for i in range(vertex_count)]

    src_sig = signatures(source_vertices)
    cap_sig = signatures(captured_vertices)
    direct_attributes = src_sig == cap_sig
    cap_map: dict[bytes, list[int]] = {}
    for index, key in enumerate(cap_sig):
        cap_map.setdefault(key, []).append(index)
    mapping = []
    unique_mapping = True
    for key in src_sig:
        candidates = cap_map.get(key, [])
        if len(candidates) != 1:
            unique_mapping = False
            mapping = []
            break
        mapping.append(candidates[0])
    if unique_mapping and len(set(mapping)) != vertex_count:
        unique_mapping = False
        mapping = []
    remapped_attributes = unique_mapping and len(mapping) == vertex_count
    src_indices = struct.unpack(f"<{model['index_count']}I", source_indices)
    cap_indices = struct.unpack(f"<{capture['index_count']}I", captured_indices)
    resolved_mapping = mapping if remapped_attributes else (
        list(range(vertex_count)) if direct_attributes else [])
    if resolved_mapping:
        remapped_index_order = tuple(resolved_mapping[index] for index in src_indices) == cap_indices
        mapped_positions = [None] * vertex_count
        source_positions = _positions(source_vertices, vertex_count, stride)
        target_positions = _positions(captured_vertices, vertex_count, stride)
        for source_index, capture_index in enumerate(resolved_mapping):
            mapped_positions[source_index] = target_positions[capture_index]
        fit = _fit_affine(source_positions, mapped_positions)
        normal_fit = _normal_affine_fit(source_vertices, captured_vertices, vertex_count,
                                        stride, resolved_mapping, fit)
    else:
        remapped_index_order = False
        fit = None
        normal_fit = None
    topology_corresponds = exact_index_bytes or remapped_index_order
    common["topology_matches"] = topology_corresponds
    if not topology_corresponds:
        return common | {
            "assessment": "unsupported-topology-comparison",
            "topology_correspondence": False,
            "notes": ["captured index connectivity does not correspond to source topology"],
        }
    structural_correspondence = (direct_attributes or remapped_attributes) and topology_corresponds
    fit_value = fit.get("normalized_rmse") if fit else None
    if exact_vertex_bytes and exact_index_bytes:
        assessment = "exact-source-bytes"
    elif (structural_correspondence and fit_value is not None and fit_value <= 1e-5 and
          normal_fit is not None and normal_fit["max_angular_error_degrees"] <= 1.0):
        assessment = "transform-consistent-correspondence"
    elif structural_correspondence:
        if fit_value is not None and fit_value <= 1e-5:
            assessment = "topology-and-attribute-correspondence; normals disagree with the position transform"
        else:
            assessment = "topology-and-attribute-correspondence; positions are non-affinely transformed or posed"
    else:
        assessment = "inconclusive"

    attr_exact = {}
    for attr_name, attr_offset, width in zip(
            ("position", "normal", "uv0", "attribute3", "tangent", "bitangent"),
            ATTRIBUTE_OFFSETS, ATTRIBUTE_WIDTHS):
        src = b"".join(source_vertices[i * stride + attr_offset:i * stride + attr_offset + width]
                        for i in range(vertex_count))
        cap = b"".join(captured_vertices[i * stride + attr_offset:i * stride + attr_offset + width]
                        for i in range(vertex_count))
        attr_exact[attr_name] = src == cap
    return common | {
        "assessment": assessment,
        "exact_vertex_array": exact_vertex_bytes,
        "exact_index_array": exact_index_bytes,
        "exact_attributes_in_runtime_order": attr_exact,
        "unique_vertex_remap_from_uv0_and_attribute3": remapped_attributes,
        "topology_correspondence": topology_corresponds,
        "position_affine_fit": fit,
        "normal_inverse_transpose_fit": normal_fit,
        "notes": [
            "UV0 plus bytes at offsets 32-47 are used only as correspondence keys; their semantics are not assumed.",
            "The affine residual does not model per-vertex skeletal deformation.",
            "A non-unique attribute key is reported as ambiguous rather than guessed.",
        ],
    }


def analyze(capture_bytes: bytes, sources: list[tuple[str, bytes]]) -> dict:
    capture = parse_capture(capture_bytes)
    return {key: value for key, value in capture.items() if key not in {"vertex_data", "index_data"}} | {
        "comparisons": [compare_source(capture, payload, label) for label, payload in sources]
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--original", type=pathlib.Path,
                        help="decrypted original tag-1 hand2 payload")
    parser.add_argument("--modified", type=pathlib.Path,
                        help="decrypted modified tag-1 hand2 payload")
    parser.add_argument("--json", type=pathlib.Path, help="write report to a local JSON file")
    args = parser.parse_args(argv)
    try:
        sources = [(label, path.read_bytes()) for label, path in
                   (("original", args.original), ("modified", args.modified)) if path]
        report = analyze(args.capture.read_bytes(), sources)
        output = json.dumps(report, indent=2) + "\n"
        if args.json:
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(output, encoding="utf-8")
        else:
            sys.stdout.write(output)
    except (OSError, ValueError, struct.error) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
