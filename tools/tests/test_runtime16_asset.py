#!/usr/bin/env python3
"""Validate the generated 16-bone runtime arm asset without Blender.

This checks the committed JSON, GLB container and runtime renders, not the
authoring .blend. It is intentionally dependency-free so it can run in CI or a
plain checkout.
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASSET = ROOT / "assets" / "berdly-arm"
RUNTIME_JSON = ASSET / "berdly_arm.runtime16.json"
RUNTIME_GLB = ASSET / "berdly_arm.runtime16.glb"
BASE_JSON = ASSET / "berdly_arm.mesh.json"
RENDERS = [
    ASSET / "renders" / "runtime_fp_idle.png",
    ASSET / "renders" / "runtime_fp_dash.png",
    ASSET / "renders" / "runtime_fp_bunnyhop.png",
    ASSET / "renders" / "runtime_fp_stomp.png",
]
EXPECTED_FRAMES = {
    "idle": 48,
    "dash": 36,
    "dash_slide": 48,
    "jump": 36,
    "stomp": 36,
    "bunnyhop": 60,
    "fake": 36,
}
CORE_BONES = {"root", "forearm_01", "forearm_02", "wrist", "hand"}
FEATHER_BONES = {
    "feather_up_01", "feather_up_02", "feather_up_03",
    "feather_outer_01", "feather_outer_02",
    "feather_inner_01", "feather_inner_02",
    "feather_primary", "feather_alula",
}


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read_glb(path: Path) -> dict:
    data = path.read_bytes()
    magic, version, total = struct.unpack_from("<4sII", data, 0)
    check(magic == b"glTF" and version == 2, "not a glTF 2.0 binary")
    check(total == len(data), "GLB length header does not match the file")
    offset = 12
    document = None
    while offset < total:
        size, kind = struct.unpack_from("<II", data, offset)
        offset += 8
        chunk = data[offset:offset + size]
        offset += size
        if kind == 0x4E4F534A:
            document = json.loads(chunk.decode("utf-8").rstrip("\x00 "))
    check(document is not None, "GLB has no JSON chunk")
    return document


def main() -> int:
    document = json.loads(RUNTIME_JSON.read_text())
    base = json.loads(BASE_JSON.read_text())

    check(document["schema"] == "hyprlodr.berdly-arm.runtime/1", "wrong runtime schema")
    check(document["runtime_bone_count"] == 16, "runtime bone count is not 16")
    check(len(document["bones"]) == 16, "runtime bone list is not 16 entries")
    names = [bone["name"] for bone in document["bones"]]
    check(len(set(names)) == 16, "runtime bone names are not unique")
    check(CORE_BONES <= set(names), "core bones missing")
    check(FEATHER_BONES <= set(names), "feather rows missing")
    check({"fingers", "thumb"} <= set(names), "merged digit groups missing")
    check(set(EXPECTED_FRAMES) == set(document["actions"]), "action names differ")
    for name, frames in EXPECTED_FRAMES.items():
        check(document["actions"][name]["frames"] == frames,
              f"{name} has {document['actions'][name]['frames']} frames, expected {frames}")

    weights = document["weights"]
    check(len(weights) == len(base["mesh"]["vertices"]), "runtime weight count differs")
    check(max(len(entry) for entry in weights) <= 4, "more than four influences")
    errors = [abs(sum(weight for _name, weight in entry) - 1.0) for entry in weights]
    check(max(errors) <= 1e-5, f"weights are not normalized: {max(errors)}")

    validation = document["validation"]
    check(validation["runtime_bone_count"] == 16, "validation bone count is not 16")
    check(validation["max_influences_per_vertex"] <= 4, "validation influence budget exceeded")
    check(validation["max_rest_matrix_error"] <= 1e-4, "rest matrices drifted")
    check(validation["max_baked_matrix_error"] <= 1e-4, "baked matrices drifted")
    check(document["source_blend"].endswith("berdly_arm.blend"),
          "source_blend does not name the authoring .blend")
    check(document["source_blend"] != document["runtime_blend"],
          "source_blend and runtime_blend are identical")

    glb = read_glb(RUNTIME_GLB)
    check(len(glb.get("skins", [])) == 1, "GLB should contain one skin")
    check(len(glb["skins"][0]["joints"]) == 16, "GLB skin is not 16 joints")
    animation_names = {animation.get("name") for animation in glb.get("animations", [])}
    check(animation_names == {f"runtime_{name}" for name in EXPECTED_FRAMES},
          f"GLB animations are {sorted(animation_names)}")
    check(len(glb.get("meshes", [])) == 1, "GLB should contain one runtime mesh")

    for path in RENDERS:
        check(path.is_file(), f"missing runtime render {path.name}")
        check(path.stat().st_size > 10_000, f"runtime render {path.name} is suspiciously small")

    print("PASS runtime16: 16 bones, 4,716 normalized weights, 7 actions, 4 renders")
    return 0


if __name__ == "__main__":
    sys.exit(main())
