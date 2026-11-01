# The first-person hand: rendering and animation pipeline

Status: **Milestone A complete; Milestone B in progress.** The material/shader path, the
skinning path and the gameplay-facing material set are identified from the shipped pack and
the executable. The live hand draw is now located and proved by suppressing it and by
substituting its vertex/index buffers. The hand is a 238-vertex, 72-byte-stride skinned mesh
behind a two-pass 1,188-index draw. A production replacement is not yet integrated: the first
test mesh was not correctly pivoted and rendered only as a fragment, and the persistent hook
has not been written. This document separates what was measured from what is inferred, and
names the next experiment.

All offsets in this file are into the **user's local installation**. No game bytes, shader
text or extracted assets are committed.

## Installation and fingerprint

| Field | Value |
|---|---|
| `hyperdemon.exe` SHA-256 | `7612dd72076002b7c0cb5cab8f7340a399176ecedefe9d3b4d0353fbdac93611` |
| Build timestamp | `0x66ecd8b5` |
| Loader fingerprint | 8/8 anchors matched at `0x140000000` |
| Renderer | OpenGL 4.6 compatibility profile, Mesa, Proton |

## Verified: the pack contains a complete material library

The live record probe finds a contiguous run of **24 `tag 8` slices** at the end of
`asset/asset`. Each decrypts with `zlib.crc32(name) & 0x3ff` and begins:

```
+0x00  "hx3_as" + 16-byte payload header
+0x18  "hx:ss" + u8 + NUL-terminated material name
```

The names are: `boss`, `dev`, `egg`, `entity`, `gem`, `gem_old`, `gui`, `hand`, `hologram`,
`lazer`, `light`, `owl`, `particle`, `portal`, `post`, `projection`, `retrieval`, `shadow`,
`skinning`, `snake`, `squid`, `thorn`, `tile`, `warp`.

This is the same set of materials named by the game's own executable strings. It is a
closed verification: the name is inside the decrypted slice and the CRC key class is derived
from the name, so an accidental match is not plausible.

### The `hand` material

`hand` is a **tag 8** material at pack offset `306,587,727`, size `1,197`, key class `0x28f`.
It defines two programs:

- `hand_vs` — position + `uv0`, `world_matrix`, `view_proj_matrix`, `spherical_proj`,
  `cam_position`, `tex_diffuse`. World position is quantized with
  `floor(world_position * 200.0) / 200.0`; `project_spherical` is applied.
- `hand_fs` — samples `tex_diffuse` at `uv0`, squares the RGB, writes it as `out_colour0`.

This material is **unskinned and texture-only**. It is not the material that produces the
arm's iridescence on its own.

### The `entity` material has dedicated hand variants

`entity` is a **tag 8** material at offset `306,470,982`, size `101,676`, key class `0x068`.
Its variant table includes:

| Variant | Programs | Defines |
|---|---|---|
| `entity_hand` | `entity_vs` / `entity_fs` | `HAND_QUANTIZE` |
| `entity_hand_vs` | `entity_vs` / `entity_fs` | `HAND_QUANTIZE`, `HAND_VS` |
| `entity_hand_position` | `entity_vs` / `position_only_fs` | `HAND_QUANTIZE` |
| `entity_hand_cns` | `entity_vs` / `cns_fs` | `CNS`, `HAND_QUANTIZE` |
| `entity_veins_cns` | `entity_vs` / `cns_fs` | `CNS`, `CNS_VEINS` |
| `entity_refract`, `entity_shiny`, `entity_ghost`, … | full entity variants | ghost/reveal/shadow/erode/phantom/threat-flare |

The `entity` vertex shader supports **linear-blend skinning with 16 bone matrices**:

```
in vec4 in_uv2;   // two bone indices
in vec4 in_uv3;   // weights; x selects the blend
uniform mat4 bone_transforms[16];
```

`skinning` (`tag 8`, offset `306,723,082`, size `1,443`, key class `0x0f6`) supplies the
three GLSL skinning helpers (`skinning4`, `skinning3`, `skinning2`, `skinning_rigid`).

The `entity_fs` hand path reads a screen-space background sample through `tex_diffuse` and
mixes it into the entity result using a model-space Y threshold:

```
hand_bg = tex_diffuse(gl_FragCoord.xy / textureSize(tex_diffuse, 0))
result1 = mix(hand_bg, result1, pow(clamp((model_y - 0.23) / -0.06, 0, 1), 0.8))
```

This is the engine's hand-specific screen blend. The material keeps its ordinary
reflection/refraction/`cns`/`spherical_proj` path, so the hand inherits the game's
iridescent entity shading rather than a flat diffuse look.

### The hand material set

The executable's material table names six hand variants:

`hand1bones`, `hand2bones`, `hand1fract`, `hand2fract`, `hand1nerves`, `hand2nerves`,
`hand1spec`, `hand2spec`, `hand1veins`, `hand2veins`, `hand1veinsgem`.

The constructor at **RVA `0x121ba0`** resolves all of these with the material resolver
(`FUN_140211bd0(ctx, name, 2)`) and stores the handles in one object. The surrounding
parameter table includes `bone_transforms`, `world_matrix`, `world_matrix_arr`,
`user_matrix_arr`, `tex_diffuse`, `tex_grad`, `tex_reflect`, `tex_mask`, `tex_edgespec`,
`tex_spec`, `tex_cns`, `spherical_proj`, `quantize_scale`, `accum_time` and `flash_white`.
Those are the uniforms of the iridescent `entity`/`shiny` path, not of the simple `hand`
pass.

### The hand texture set

The static constructor at **RVA `0x59e90`** resolves the type-1 (texture) assets:

`hand2`, `hand2left`, `handbones`, `handbonesleft`, `handveins`, `handveinsleft`,
`handnerves`, `handnervesleft`

and the type-2 fracture materials `hand1fract`, `hand2fract`. It also initialises the
player action names `SPRINT`, `SLIDE`, `DASH_PRE`, `FASTFALL`.

A second constructor at **RVA `0x1a3e70`** resolves the same hand textures and stores them
in a per-player hand object (`param_1[0x52]..[0x59]`). `hand2left` appears at
**RVA `0x1a58d6`**.

The matching material variants and textures strongly indicate a **two-sided hand model**
(right/left) with separate bone, nerve, vein, specular and fracture passes, skinned by the
16-matrix `entity` path.

### The skinning uniform

**RVA `0xf8ec0`** writes `bone_matrices` (as named by the executable). The material
constructor at `0x121ba0` uses `bone_transforms` for the object-space transform array and
`bone_transforms`/`world_matrix_arr` in the per-instance parameter table. This is the
standard 16-bone LBS path.

## Verified live: the hand draw and its buffers

The engine does not resolve `glDrawElements` through an import slot. At
`hyperdemon.exe+0x2370c0` it performs an indirect call through its own dispatch table at
**RVA `0x3d9ac8`**; the pointer lands on the PE `opengl32` thunk at `0x7bee1458` in the live
Proton process. That explains why patching `GetProcAddress`, `wglGetProcAddress` and direct
imports saw no draw calls.

Host GDB attached to the real process and broke at the thunk. The draw stream is 126
`glDrawElements` calls per frame. Two adjacent calls with `count = 1188` are the hand:

- zeroing only that count removed the entire first-person arm while the world kept rendering;
- restoring it brought the arm back;
- the same two calls are the only 1,188-index draws in the frame.

At the hand draw the measured state is:

| Item | Value |
|---|---|
| Mode | `GL_TRIANGLES` (4) |
| Count | 1,188 indices per pass; two passes per frame |
| Index type | `GL_UNSIGNED_SHORT` (5125, read from the call setup) |
| Attribute buffer | 41 (all six enabled attributes) |
| Element buffer | 42 |
| Attribute buffer size | 17,136 bytes |
| Element buffer size | 4,752 bytes |
| Vertex stride | 72 bytes |
| Vertex count | 238 (`17136 / 72`) |
| Enabled attributes | six: 3, 3, 2, 4, 3, 3 floats, all stride 72, all in buffer 41 |
| Attribute offsets | 0, 12, 24, 32, 48, 60 bytes |

The first three floats of each 72-byte record are object-space position; the next three are
normal. The remaining fields carry tangent/UV and the skinned-entity data. The 4,752-byte
index buffer holds two 1,188-index runs, consistent with two passes (or right/left hands).

**Buffer substitution is proved, but a usable replacement is not.** Writing an eight-vertex
cube in the same 72-byte format and 36 indices, then changing only the hand draw to
`count = 36`, removed the arm and produced a small fragment at the centre of the screen.
When the original index count was left at 1,188 while the first records were replaced, the
arm visibly stretched into large triangles. Both outcomes prove the draw consumes buffer 41
and index buffer 42 live; neither is a correct rig-space placement. Reversing every cube
triangle to face outward produced the same fragment, so back-face culling is not the cause.
A second live process confirmed the same 41/42 buffers and an index pointer of **NULL**
(offset zero); a cube with a 0.5 half-size at the bounding-box centre still rendered only
as illuminated fragments. That rules out an index offset and back-face culling, but not the
inherited UV/bone/material state. The cube inherited one existing vertex's data rather than
a deliberate hand pivot, and the draw was not restored before the first capture window
ended.

## Verified: gameplay-facing state names

The executable/pack contain the action vocabulary used by the player:

`SPRINT`, `SLIDE`, `DASH_PRE`, `FASTFALL`, plus the audio/event keys `airdash`, `drift`,
`fake`, `jump1..3`, `land`, `land2`, `turn`, `perfect`, `rapid1..4`, `playersmash`,
`smallsmash`, `gather`/`grab`, `throw`, `shot2_*`, `shot3_*`, `dodge`, `fall`, `move`.

The hand constructor `0x59e90` is co-located with `SPRINT`, `SLIDE`, `DASH_PRE` and
`FASTFALL`, so the hand's visual state machine is driven by the same locomotion state names
the audio/event system uses. This is the hook vocabulary the feather runtime should consume.

## Inferred (not yet proven)

- The 238-vertex buffer is a real skinned hand mesh. It is not yet proven whether the two
  1,188-index passes are duplicate passes, different material layers, or right/left hands;
  the index buffer contains two complete runs.
- `hand2`/`hand2left` are texture atlases, not mesh names. They appear through the type-1
  resolver in `0x59e90`, while every hand material goes through the type-2 resolver.
- The iridescence is produced by the `entity` material family (reflection/refraction,
  `tex_reflect`, `tex_edgespec`, `spherical_proj`, `accum_time`, `iri`), not by the
  simple `hand` material. Reusing `entity_hand`/`entity_hand_vs` is the correct way to
  preserve it.
- The source asset/record name is still unknown. The live buffer gives geometry and a draw
  signature, not a named pack record.

## Remaining unknowns and the integration blocker

1. **Pass identity.** The two `count=1188` draws share the same buffers. They could be a
   depth/material pair or right/left hands; distinguishing them requires logging the program,
   uniforms and texture set at each pass.
2. **Bone palette and program state.** Attribute offsets are now measured
   (`0,12,24,32,48,60`), but the current program and the `bone_transforms` uniform block are
   not captured reliably. Capture those before building an independent VAO; the test cube
   already uses the measured offsets and still needs a deliberate pivot.
3. **Persistent hook.** The live proof uses GDB. The production path is to patch the 8-byte
   pointer at RVA `0x3d9ac8` after fingerprint verification, save the original and restore it
   on unload. Startup may rewrite the table, so the hook must install/verify at the first
   frame and gate on the `count=1188`/VAO/buffer signature.
4. **16-bone bake.** The authored arm still has 26 bones; the `entity` path supplies at most
   16 matrices. This is independent of the draw hook and remains required before integration.

## Replacement requirements (derived from the verified pipeline)

A replacement that renders through the game's own arm path must provide:

- **Vertex attributes**: `in_position`, `in_normal`, `in_tangent`, `in_uv0`, `in_uv1`,
  and for the skinned path `in_uv2` (two bone indices) and `in_uv3` (weights).
- **Bone budget**: at most 16 matrices in `bone_transforms`.
- **Space**: same object/world scale as the original mesh; the hand pass quantizes world
  position to 1/200 and the screen blend keys off model-space Y (`0.23` threshold).
- **Materials**: material slots that resolve to the `entity` hand variants
  (`entity_hand`, `entity_hand_vs`, `entity_hand_position`, `entity_hand_cns`,
  `entity_veins_cns`) and the hand texture set, not a new unlit material.
- **Two hands**: right and left variants if the game renders both (the asset set does).
- **Animation**: the hand object must be driven by the same world/view matrices and
  `bone_transforms` the game already supplies; a replacement cannot assume a fixed camera.

## Asset authoring and runtime-bake status (Milestone C, runtime asset complete)

The independently authored replacement is generated by
`tools/assets/build_berdly_arm.py`; no game mesh, texture or shader bytes are used. The
current artifact is a **validated 16-bone runtime asset, not an in-game replacement**.

Verified by the generated `berdly_arm.mesh.json` (2.7 MB, schema
`hyprlodr.berdly-arm/1`):

| Property | Value |
|---|---|
| Vertices / faces | 4,716 / 4,570 |
| Authoring bones | 26 (root, 4 arm/hand, 13 finger/claw, 9 feather rows) |
| Max bone influences per vertex | 3 |
| Weight normalization error | <= 1.2e-16; no unknown bone names |
| Faces with UVs | 4,570 / 4,570 |
| Baked actions | idle 48, dash 36, dash_slide 48, jump 36, stomp 36, bunnyhop 60, fake 36 frames |
| Runtime spring | `native/src/feather_spring.c`, fixed 240 Hz substeps, bounded, reset API |

**16-bone runtime bake (verified).** `tools/assets/bake_runtime16.py` opens the editable
`.blend` and creates a second armature/mesh in a `Runtime16` collection. It keeps the five
core bones and all nine feather rows, and merges the three fingers plus thumb into two rigid
digit groups, exactly filling the 16-matrix budget.

| Runtime property | Value |
|---|---|
| Runtime bones | 16 (5 core + 2 merged digits + 9 feather rows) |
| Max influences per vertex | 3 |
| Weight-sum error | 3.0e-8 |
| Runtime rest-matrix error | 1.8e-7 |
| Baked pose-matrix error | 2.7e-7 |
| Runtime actions | idle, dash, dash_slide, jump, stomp, bunnyhop, fake |
| GLB animations | exactly seven `runtime_*` animations |
| Runtime renders | `renders/runtime_fp_{idle,dash,bunnyhop,stomp}.png` |

The runtime outputs are `berdly_arm.runtime16.blend`, `.glb` and `.json`; the original
26-bone authoring rig and actions remain in `berdly_arm.blend`. The merged digit groups are
rigid in the runtime (individual finger curls are authoring-only); all feather rows remain
independently animated. Visual inspection of the four runtime renders confirms the blue
faceted arm, layered feather rows and claws survive the bake, but no game draw path uses
these objects yet.

Visual iteration was inspected from the generated renders in
`assets/berdly-arm/renders/`: the arm reads as a deep-blue, faceted, clawed avian limb with
layered vanes, visible iridescent blue/violet highlights and dark hooked claws. The framing
is still an art-direction approximation of the game camera, and none of this is game-render
verification. The current first-person render still has substantial empty frame around the
arm and the vanes read more like sharp plates than fine barbs; both are open art issues.

## Next experiments, in order

1. **Write the persistent dispatch hook.** After fingerprint verification, replace the
   pointer at RVA `0x3d9ac8` with a shim that saves the original, logs the hand signature and
   can temporarily set the hand count to zero. Install it from a mod or the proxy, never
   from a global import patch. Prove that turning it off restores the original arm.
2. **Capture the missing state.** At the `count=1188` draw, log the current program, the
   six attribute offsets, the index-buffer offset/type, the `bone_transforms` uniform block
   and the material/texture bindings. This turns the buffer layout into a complete VAO spec.
3. **Build a valid test mesh.** Use a deliberate hand pivot and deliberately chosen bone
   weights, not an arbitrary existing vertex. Verify a solid, correctly lit cube at the hand
   before attempting the authored arm. Reversibility is part of the test: restore buffers and
   count on unload.
4. **Integrate the baked 16-bone runtime asset.** The runtime mesh, weights and seven actions
   are now validated; feed their per-frame bone matrices into the hand draw and bind the
   game's `entity_hand`/`entity_hand_vs` material. Preserve the screen-space hand blend and
   iridescence.
5. **Verify in gameplay.** Exercise idle, dash, dash-slide, jump-out-of-slide, stomp,
   bunnyhop/fake and abrupt direction changes, then restore the original arm when the mod is
   disabled.

## Reproducible tools

```bash
# enumerate the material library and verify the CRC key classes
python3 tools/re/name_records.py --game "$GAME" --json /tmp/assets.json

# decrypt one material (no bytes written by default)
python3 tools/re/pack_crypt.py --exe "$GAME/hyperdemon.exe" --pack "$GAME/asset/asset" \
    --name hand --offset 306587727 --size 1197 --hex

# inspect a locally dumped hand VBO/IBO and manufacture a same-format test cube
python3 tools/re/hand_vbo_test.py --vbo /tmp/hand-vbo.bin --ibo /tmp/hand-ibo.bin \
    --out /tmp/hand-test

# the replacement mesh/rig used for the visual target
blender -b --python tools/assets/build_berdly_arm.py

# bake the validated 16-bone runtime variant and its renders
blender -b assets/berdly-arm/berdly_arm.blend \
    --python tools/assets/bake_runtime16.py

# validate the committed runtime JSON/GLB/renders without Blender
python3 tools/tests/test_runtime16_asset.py
```

The VBO/IBO dumps contain game geometry and must stay outside the repository. The helper
only writes derived test data and prints layout facts; it never embeds the dump.

The asset generator is independent of any game content: every mesh, feather, bone, weight
and material is authored in the script.
