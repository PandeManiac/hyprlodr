#!/usr/bin/env python3
"""Bake the authored Berdly arm into the game's 16-bone runtime budget.

The editable authoring rig has 26 bones: five arm/hand bones, twelve finger/claw
bones and nine feather rows. The verified entity draw path supplies at most 16
bone matrices. This bake keeps all nine feather rows and the five core bones,
and merges the three fingers plus the thumb into two rigid digit groups:

    core (5) + fingers + thumb + feather rows (9) = 16

The 26-bone armature, mesh and actions remain untouched in the .blend. The
runtime objects live in a *Runtime16* collection and are exported separately.

Usage (after tools/assets/build_berdly_arm.py has generated the .blend):
    blender -b assets/berdly-arm/berdly_arm.blend \
        --python tools/assets/bake_runtime16.py

Outputs:
    assets/berdly-arm/berdly_arm.runtime16.blend
    assets/berdly-arm/berdly_arm.runtime16.glb
    assets/berdly-arm/berdly_arm.runtime16.json
    assets/berdly-arm/renders/runtime_fp_idle.png
    assets/berdly-arm/renders/runtime_fp_dash.png
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Matrix, Vector

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "assets" / "berdly-arm"
BASE_JSON = OUT / "berdly_arm.mesh.json"
RUNTIME_BLEND = OUT / "berdly_arm.runtime16.blend"
RUNTIME_GLB = OUT / "berdly_arm.runtime16.glb"
RUNTIME_JSON = OUT / "berdly_arm.runtime16.json"
RENDERS = OUT / "renders"
COLLECTION_NAME = "Runtime16"
RIG_NAME = "BerdlyArmRuntime"
MESH_NAME = "BerdlyArmRuntimeMesh"

CORE_BONES = ["root", "forearm_01", "forearm_02", "wrist", "hand"]
FEATHER_BONES = [
    "feather_up_01", "feather_up_02", "feather_up_03",
    "feather_outer_01", "feather_outer_02",
    "feather_inner_01", "feather_inner_02",
    "feather_primary", "feather_alula",
]
RUNTIME_ORDER = CORE_BONES + ["fingers", "thumb"] + FEATHER_BONES
assert len(RUNTIME_ORDER) == 16 and len(set(RUNTIME_ORDER)) == 16

# Each runtime bone samples one source bone. The two merged digit groups use the
# second joint so the authored curl still reaches the claws; all finger vertices
# move as one piece because the runtime only has one matrix per group.
SOURCE_BONE = {name: name for name in CORE_BONES + FEATHER_BONES}
SOURCE_BONE["fingers"] = "finger_b_02"
SOURCE_BONE["thumb"] = "thumb_02"

PARENT = {
    "root": None,
    "forearm_01": "root",
    "forearm_02": "forearm_01",
    "wrist": "forearm_02",
    "hand": "wrist",
    "fingers": "hand",
    "thumb": "hand",
    "feather_up_03": "wrist",
    "feather_primary": "wrist",
    "feather_alula": "wrist",
    "feather_up_02": "forearm_02",
    "feather_outer_02": "forearm_02",
    "feather_inner_02": "forearm_02",
    "feather_up_01": "forearm_01",
    "feather_outer_01": "forearm_01",
    "feather_inner_01": "forearm_01",
}

MERGED_GROUPS = {
    "fingers": [
        "finger_a_01", "finger_a_02", "claw_a",
        "finger_b_01", "finger_b_02", "claw_b",
        "finger_c_01", "finger_c_02", "claw_c",
    ],
    "thumb": ["thumb_01", "thumb_02", "claw_t"],
}
GROUP_MAP = {
    **{name: name for name in CORE_BONES + FEATHER_BONES},
    **{source: runtime for runtime, sources in MERGED_GROUPS.items() for source in sources},
}
ACTION_NAMES = ["idle", "dash", "dash_slide", "jump", "stomp", "bunnyhop", "fake"]
FP_CAMERA = ((0.120, -0.980, 0.300), (0.120, 0.320, 0.100))
RENDER_SHOTS = [
    ("idle", 14, "runtime_fp_idle", FP_CAMERA),
    ("dash", 12, "runtime_fp_dash", FP_CAMERA),
    ("bunnyhop", 22, "runtime_fp_bunnyhop", FP_CAMERA),
    ("stomp", 10, "runtime_fp_stomp", FP_CAMERA),
]


def log(message: str) -> None:
    print(f"[runtime16] {message}")


def remove_existing() -> None:
    for name in (RIG_NAME, MESH_NAME):
        obj = bpy.data.objects.get(name)
        if obj:
            bpy.data.objects.remove(obj, do_unlink=True)
    for action in list(bpy.data.actions):
        if action.name.startswith("berdly_runtime_"):
            bpy.data.actions.remove(action)
    collection = bpy.data.collections.get(COLLECTION_NAME)
    if collection:
        for obj in list(collection.objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        bpy.data.collections.remove(collection)
    for armature in list(bpy.data.armatures):
        if armature.name.startswith(RIG_NAME):
            bpy.data.armatures.remove(armature)
    for mesh in list(bpy.data.meshes):
        if mesh.name.startswith(MESH_NAME):
            bpy.data.meshes.remove(mesh)


def read_source_rolls(source_arm) -> dict[str, float]:
    bpy.context.view_layer.objects.active = source_arm
    bpy.ops.object.select_all(action="DESELECT")
    source_arm.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    rolls = {bone.name: bone.roll for bone in source_arm.data.edit_bones}
    bpy.ops.object.mode_set(mode="OBJECT")
    return rolls


def create_runtime_armature(source_arm, collection, source_rolls):
    data = bpy.data.armatures.new(RIG_NAME)
    obj = bpy.data.objects.new(RIG_NAME, data)
    collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    edit_bones = {}
    for name in RUNTIME_ORDER:
        source_name = SOURCE_BONE[name]
        source = source_arm.data.bones[source_name]
        bone = data.edit_bones.new(name)
        bone.head = source.head_local
        bone.tail = source.tail_local
        bone.roll = source_rolls[source_name]
        bone.use_deform = True
        bone.use_connect = False
        parent = PARENT[name]
        if parent:
            bone.parent = edit_bones[parent]
        edit_bones[name] = bone
    bpy.ops.object.mode_set(mode="OBJECT")
    data.display_type = "STICK"
    obj.show_in_front = True
    for pose_bone in obj.pose.bones:
        pose_bone.rotation_mode = "QUATERNION"
    return obj


def rest_matrix_error(source_arm, runtime_arm) -> float:
    error = 0.0
    for name in RUNTIME_ORDER:
        source = source_arm.data.bones[SOURCE_BONE[name]].matrix_local
        runtime = runtime_arm.data.bones[name].matrix_local
        for row in range(4):
            for column in range(4):
                error = max(error, abs(source[row][column] - runtime[row][column]))
    return error


def create_runtime_mesh(source_mesh, runtime_arm, collection):
    mesh = source_mesh.data.copy()
    mesh.name = MESH_NAME
    obj = bpy.data.objects.new(MESH_NAME, mesh)
    collection.objects.link(obj)
    obj.parent = runtime_arm
    obj.matrix_parent_inverse = Matrix.Identity(4)
    obj.matrix_world = source_mesh.matrix_world.copy()
    obj.vertex_groups.clear()
    groups = {name: obj.vertex_groups.new(name=name) for name in RUNTIME_ORDER}

    source_groups = {group.index: group.name for group in source_mesh.vertex_groups}
    max_influences = 0
    weight_error = 0.0
    covered = set()
    for vertex in source_mesh.data.vertices:
        aggregate: dict[str, float] = {}
        for entry in vertex.groups:
            source_name = source_groups.get(entry.group)
            target = GROUP_MAP.get(source_name)
            if target is None:
                continue
            covered.add(source_name)
            aggregate[target] = aggregate.get(target, 0.0) + entry.weight
        entries = sorted(aggregate.items(), key=lambda item: item[1], reverse=True)[:4]
        total = sum(weight for _name, weight in entries)
        if total <= 1e-9:
            raise RuntimeError(f"vertex {vertex.index} has no runtime weights")
        max_influences = max(max_influences, len(entries))
        weight_error = max(weight_error, abs(total - 1.0))
        for name, weight in entries:
            groups[name].add([vertex.index], weight / total, "REPLACE")

    missing = set(source_groups.values()) - covered
    if missing:
        raise RuntimeError(f"source groups with no vertices: {sorted(missing)}")

    for modifier in list(obj.modifiers):
        obj.modifiers.remove(modifier)
    modifier = obj.modifiers.new("Runtime Armature", "ARMATURE")
    modifier.object = runtime_arm
    obj.hide_render = True
    obj.hide_viewport = True
    return obj, max_influences, weight_error


def reset_pose(arm, *, quaternion: bool) -> None:
    for pose_bone in arm.pose.bones:
        pose_bone.location = (0.0, 0.0, 0.0)
        pose_bone.scale = (1.0, 1.0, 1.0)
        if quaternion:
            pose_bone.rotation_mode = "QUATERNION"
            pose_bone.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
        else:
            pose_bone.rotation_euler = (0.0, 0.0, 0.0)


def matrix_to_list(matrix: Matrix) -> list[list[float]]:
    return [[float(matrix[row][column]) for column in range(4)] for row in range(4)]


def bake_actions(source_arm, runtime_arm):
    scene = bpy.context.scene
    source_arm.data.pose_position = "POSE"
    runtime_arm.data.pose_position = "POSE"
    source_arm.animation_data_create()
    runtime_arm.animation_data_create()
    actions = {}
    samples = {}
    max_matrix_error = 0.0
    bone_errors = {name: 0.0 for name in RUNTIME_ORDER}

    for name in ACTION_NAMES:
        source_action = bpy.data.actions.get(f"berdly_{name}")
        if source_action is None:
            raise RuntimeError(f"missing source action berdly_{name}")
        start, end = (int(round(value)) for value in source_action.frame_range)
        source_arm.animation_data.action = source_action
        action = bpy.data.actions.new(f"berdly_runtime_{name}")
        action.use_fake_user = True
        runtime_arm.animation_data.action = action
        action_samples = {bone: {"location": [], "rotation_quaternion": [], "scale": []}
                          for bone in RUNTIME_ORDER}
        log(f"baking {name}: frames {start}..{end}")

        for frame in range(start, end + 1):
            reset_pose(source_arm, quaternion=False)
            scene.frame_set(frame)
            bpy.context.view_layer.update()
            reset_pose(runtime_arm, quaternion=True)
            for bone_name in RUNTIME_ORDER:
                source_name = SOURCE_BONE[bone_name]
                runtime_arm.pose.bones[bone_name].matrix = \
                    source_arm.pose.bones[source_name].matrix.copy()
                # The runtime parent can differ from the source parent (the merged
                # digit groups parent directly to the hand). Update the dependency
                # graph so the next child is solved against the new parent pose.
                bpy.context.view_layer.update()

            for bone_name in RUNTIME_ORDER:
                runtime_bone = runtime_arm.pose.bones[bone_name]
                source_matrix = source_arm.pose.bones[SOURCE_BONE[bone_name]].matrix
                for row in range(4):
                    for column in range(4):
                        error = abs(
                            runtime_bone.matrix[row][column] - source_matrix[row][column])
                        max_matrix_error = max(max_matrix_error, error)
                        bone_errors[bone_name] = max(bone_errors[bone_name], error)
                channel = action_samples[bone_name]
                channel["location"].append([float(value) for value in runtime_bone.location])
                channel["rotation_quaternion"].append(
                    [float(value) for value in runtime_bone.rotation_quaternion])
                channel["scale"].append([float(value) for value in runtime_bone.scale])
                runtime_bone.keyframe_insert("location", frame=frame, group=bone_name)
                runtime_bone.keyframe_insert(
                    "rotation_quaternion", frame=frame, group=bone_name)
                runtime_bone.keyframe_insert("scale", frame=frame, group=bone_name)

        action.use_frame_range = True
        action.frame_start = float(start)
        action.frame_end = float(end)
        actions[name] = (action, start, end)
        samples[name] = action_samples

    reset_pose(source_arm, quaternion=False)
    reset_pose(runtime_arm, quaternion=True)
    source_arm.animation_data.action = bpy.data.actions["berdly_idle"]
    runtime_arm.animation_data.action = actions["idle"][0]
    scene.frame_set(1)
    bpy.context.view_layer.update()
    log("matrix error by bone: " + ", ".join(
        f"{name}={bone_errors[name]:.3g}" for name in sorted(bone_errors, key=bone_errors.get,
                                                             reverse=True)[:6]))
    return actions, samples, max_matrix_error


def render_runtime(source_mesh, runtime_mesh, runtime_arm, actions):
    scene = bpy.context.scene
    camera = bpy.data.objects.get("BerdlyCam")
    if camera is None:
        raise RuntimeError("BerdlyCam not found; cannot render runtime views")
    RENDERS.mkdir(parents=True, exist_ok=True)
    source_mesh.hide_render = True
    runtime_mesh.hide_render = False
    runtime_mesh.hide_viewport = False
    for action_name, frame, filename, (location, target) in RENDER_SHOTS:
        runtime_arm.animation_data.action = actions[action_name][0]
        scene.frame_set(frame)
        camera.location = location
        direction = Vector(target) - Vector(location)
        camera.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
        bpy.context.view_layer.update()
        scene.render.filepath = str(RENDERS / f"{filename}.png")
        bpy.ops.render.render(write_still=True)
        log(f"rendered {filename}")
    runtime_mesh.hide_render = True
    runtime_mesh.hide_viewport = True
    source_mesh.hide_render = False


def select_runtime(runtime_arm, runtime_mesh) -> None:
    runtime_mesh.hide_viewport = False
    runtime_mesh.hide_render = False
    bpy.ops.object.select_all(action="DESELECT")
    runtime_arm.select_set(True)
    runtime_mesh.select_set(True)
    bpy.context.view_layer.objects.active = runtime_arm


def export_runtime(runtime_arm, runtime_mesh, actions) -> None:
    select_runtime(runtime_arm, runtime_mesh)
    animation_data = runtime_arm.animation_data
    previous_action = animation_data.action
    animation_data.action = None
    tracks = []
    for name in ACTION_NAMES:
        action, start, _end = actions[name]
        track = animation_data.nla_tracks.new()
        track.name = f"runtime_{name}"
        strip = track.strips.new(name, int(start), action)
        strip.name = name
        tracks.append(track)
    try:
        bpy.ops.export_scene.gltf(
            filepath=str(RUNTIME_GLB), export_format="GLB", use_selection=True,
            export_animations=True, export_animation_mode="NLA_TRACKS",
            export_nla_strips=True,
        )
    finally:
        for track in tracks:
            animation_data.nla_tracks.remove(track)
        animation_data.action = previous_action
    log(f"wrote {RUNTIME_GLB}")


def write_json(source_arm, runtime_arm, runtime_mesh, max_influences, weight_error,
               max_matrix_error, samples):
    base = json.loads(BASE_JSON.read_text())
    bones = []
    for name in RUNTIME_ORDER:
        bone = runtime_arm.data.bones[name]
        bones.append({
            "name": name,
            "source": SOURCE_BONE[name],
            "parent": PARENT[name],
            "head": [float(value) for value in bone.head_local],
            "tail": [float(value) for value in bone.tail_local],
            "matrix": matrix_to_list(bone.matrix_local),
        })

    groups = {group.index: group.name for group in runtime_mesh.vertex_groups}
    weights = []
    for vertex in runtime_mesh.data.vertices:
        entries = sorted(
            ((entry.weight, groups[entry.group]) for entry in vertex.groups
             if entry.weight > 1e-5),
            reverse=True,
        )
        total = sum(weight for weight, _name in entries) or 1.0
        weights.append([[name, weight / total] for weight, name in entries])

    runtime_actions = {}
    for name, sample in samples.items():
        runtime_actions[name] = {
            "frames": len(sample[RUNTIME_ORDER[0]]["location"]),
            "channels": sample,
        }

    payload = {
        "schema": "hyprlodr.berdly-arm.runtime/1",
        "generated_by": "tools/assets/bake_runtime16.py",
        "source_blend": str((OUT / "berdly_arm.blend").relative_to(ROOT)),
        "runtime_blend": str(RUNTIME_BLEND.relative_to(ROOT)),
        "source_mesh": "BerdlyArmMesh",
        "source_armature": "BerdlyArmRig",
        "runtime_mesh": MESH_NAME,
        "runtime_armature": RIG_NAME,
        "units": base["units"],
        "bone_budget": 16,
        "authoring_bone_count": 26,
        "runtime_bone_count": len(bones),
        "bones": bones,
        "weights": weights,
        "feather_bones": FEATHER_BONES,
        "merged_groups": MERGED_GROUPS,
        "actions": runtime_actions,
        "springs": base["springs"],
        "game_material_targets": base["game_material_targets"],
        "validation": {
            "max_influences_per_vertex": max_influences,
            "max_weight_sum_error": weight_error,
            "max_rest_matrix_error": rest_matrix_error(
                bpy.data.objects["BerdlyArmRig"], runtime_arm),
            "max_baked_matrix_error": max_matrix_error,
            "runtime_bone_count": len(bones),
        },
        "notes": [
            "All nine authored feather rows are preserved as independent runtime bones.",
            "The three fingers and the thumb are merged into two rigid digit groups to fit "
            "the 16-matrix entity budget; individual finger curls are an authoring-only detail.",
            "Runtime weights are normalized and capped at four influences per vertex.",
            "The .blend retains the complete 26-bone authoring rig and actions.",
        ],
    }
    RUNTIME_JSON.write_text(json.dumps(payload, indent=1) + "\n")
    # The authoring payload is still the source of the base geometry, but its
    # runtime skinning note must not claim the bake is still pending.
    base["runtime_skinning_status"] = "baked-16"
    base["runtime_bone_count"] = 16
    base["runtime_asset"] = "berdly_arm.runtime16.json"
    BASE_JSON.write_text(json.dumps(base, indent=1) + "\n")
    log(f"wrote {RUNTIME_JSON} and updated {BASE_JSON.name}")


def main():
    if "--no-render" in sys.argv:
        log("render pass disabled")

    source_mesh = bpy.data.objects.get("BerdlyArmMesh")
    source_arm = bpy.data.objects.get("BerdlyArmRig")
    if source_mesh is None or source_arm is None:
        raise RuntimeError("BerdlyArmMesh/BerdlyArmRig not found in the .blend")

    remove_existing()
    source_rolls = read_source_rolls(source_arm)
    collection = bpy.data.collections.new(COLLECTION_NAME)
    bpy.context.scene.collection.children.link(collection)
    runtime_arm = create_runtime_armature(source_arm, collection, source_rolls)
    runtime_mesh, max_influences, weight_error = create_runtime_mesh(
        source_mesh, runtime_arm, collection)

    rest_error = rest_matrix_error(source_arm, runtime_arm)
    log(f"runtime bones: {len(runtime_arm.data.bones)}")
    log(f"max influences: {max_influences}; weight error: {weight_error:.3g}; "
        f"rest error: {rest_error:.3g}")
    if len(runtime_arm.data.bones) != 16:
        raise RuntimeError("runtime armature is not 16 bones")
    if rest_error > 1e-4:
        raise RuntimeError(f"runtime rest matrices differ by {rest_error}")

    actions, samples, matrix_error = bake_actions(source_arm, runtime_arm)
    log(f"max baked matrix error: {matrix_error:.3g}")
    if matrix_error > 1e-4:
        raise RuntimeError(f"baked pose matrices differ by {matrix_error}")

    if "--no-render" not in sys.argv:
        render_runtime(source_mesh, runtime_mesh, runtime_arm, actions)
    export_runtime(runtime_arm, runtime_mesh, actions)
    write_json(source_arm, runtime_arm, runtime_mesh, max_influences, weight_error,
               matrix_error, samples)

    # The editable file keeps both rigs. The runtime collection is hidden by
    # default so opening the .blend shows the authoring arm.
    runtime_mesh.hide_render = True
    runtime_mesh.hide_viewport = True
    bpy.ops.object.select_all(action="DESELECT")
    source_arm.select_set(True)
    bpy.context.view_layer.objects.active = source_arm
    bpy.ops.wm.save_as_mainfile(filepath=str(RUNTIME_BLEND))
    log(f"saved {RUNTIME_BLEND}")


if __name__ == "__main__":
    main()
