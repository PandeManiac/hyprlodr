#!/usr/bin/env python3
"""Validate files emitted by the compiled Windows capture writer with production CLI."""
from __future__ import annotations

import hashlib
import json
import pathlib
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
CLI = ROOT / "tools/re/hand_capture.py"
HEADER = struct.Struct("<8s12I2Q8I")


def analyze(path: pathlib.Path) -> tuple[subprocess.CompletedProcess[str], dict | None]:
    result = subprocess.run([sys.executable, str(CLI), str(path)],
                            text=True, capture_output=True, check=False)
    return result, json.loads(result.stdout) if result.returncode == 0 else None


def reject(root: pathlib.Path, name: str, data: bytes) -> None:
    invalid_root = root / "invalid"
    invalid_root.mkdir(exist_ok=True)
    path = invalid_root / f"{name}.capture"
    path.write_bytes(data)
    result, _ = analyze(path)
    assert result.returncode != 0, f"production CLI accepted invalid case {name}"


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: native_capture_integration.py <native-fixture-output-dir>")
    root = pathlib.Path(sys.argv[1])
    for name, vertices, indices, vertex_bytes, index_bytes, offset in (
            ("original", 238, 1188, 17136, 4752, 0),
            ("enlarged", 239, 1191, 17208, 4764, 12)):
        capture_path = root / f"{name}.capture"
        expected_path = root / f"{name}.expected"
        raw = capture_path.read_bytes()
        expected = expected_path.read_bytes()
        result, report = analyze(capture_path)
        assert result.returncode == 0, result.stderr
        assert report is not None
        assert len(raw) == HEADER.size + vertex_bytes + index_bytes
        header = HEADER.unpack_from(raw)
        assert (header[6], header[8], header[9], header[10], header[11],
                header[12], header[13]) == (
                    indices, vertices, 72, vertex_bytes, indices, 4, offset)
        assert header[2] == HEADER.size and header[5] == 7 and header[7] == 0x1405
        assert header[15:17] == (41, 42)
        assert header[17:] == (0, 12, 24, 32, 48, 60)
        vertex_data = raw[HEADER.size:HEADER.size + vertex_bytes]
        index_data = raw[HEADER.size + vertex_bytes:]
        assert expected == vertex_data + index_data
        assert report["vertex_count"] == vertices
        assert report["index_count"] == report["draw_count"] == indices
        assert report["vertex_bytes"] == vertex_bytes
        assert report["index_offset_bytes"] == offset
        assert report["vertex_sha256"] == hashlib.sha256(vertex_data).hexdigest()
        assert report["index_sha256"] == hashlib.sha256(index_data).hexdigest()
        assert report["array_buffer_id"] == 41 and report["element_buffer_id"] == 42
        assert report["attribute_offsets"] == [0, 12, 24, 32, 48, 60]
        unpacked_indices = struct.unpack(f"<{indices}I", index_data)
        assert max(unpacked_indices) < vertices
        if name == "enlarged":
            assert report["extra_vertex_referenced"] is True
            assert report["extra_vertex_sha256"] == hashlib.sha256(
                vertex_data[238 * 72:239 * 72]).hexdigest()
            assert report["final_triangle_indices"] == [238, 1, 2]
            assert unpacked_indices[-3:] == (238, 1, 2)
        print(f"native {name}: {vertices} vertices/{indices} indices, "
              f"{vertex_bytes} VBO bytes, {index_bytes} IBO bytes, "
              f"sha256={report['vertex_sha256']}/{report['index_sha256']}")

    source = (root / "enlarged.capture").read_bytes()
    vertex_bytes = 239 * 72
    cases = {
        "short-header": source[:HEADER.size - 1],
        "short-vbo": source[:HEADER.size + vertex_bytes - 1],
        "short-ibo": source[:-1],
        "trailing": source + b"x",
        "magic": b"BADMAGIC" + source[8:],
        "version": source[:8] + struct.pack("<I", 2) + source[12:],
        "logical-length": source[:44] + struct.pack("<I", vertex_bytes - 72) + source[48:],
        "unsupported-count": source[:36] + struct.pack("<I", 240) + source[40:],
        "attribute-layout": source[:80] + struct.pack("<I", 56) + source[84:],
    }
    invalid_index = bytearray(source)
    struct.pack_into("<I", invalid_index, len(invalid_index) - 4, 239)
    cases["index-out-of-range"] = bytes(invalid_index)
    for name, data in cases.items():
        reject(root, name, data)
    print(f"production parser rejected {len(cases)} malformed native capture variants")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
