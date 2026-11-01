# The first-person hand: rendering and animation pipeline

Status: **Milestone A, partial and evidence-based.** The material/shader path, the skinning
path and the gameplay-facing material set are identified from the shipped pack and the
executable. The geometry source is **not yet proven**: there is no named hand mesh in the
records recovered so far, and the renderer's draw calls are still invisible to the in-process
capture. This document separates what was measured from what is inferred, and names the next
experiment.

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

## Verified: gameplay-facing state names

The executable/pack contain the action vocabulary used by the player:

`SPRINT`, `SLIDE`, `DASH_PRE`, `FASTFALL`, plus the audio/event keys `airdash`, `drift`,
`fake`, `jump1..3`, `land`, `land2`, `turn`, `perfect`, `rapid1..4`, `playersmash`,
`smallsmash`, `gather`/`grab`, `throw`, `shot2_*`, `shot3_*`, `dodge`, `fall`, `move`.

The hand constructor `0x59e90` is co-located with `SPRINT`, `SLIDE`, `DASH_PRE` and
`FASTFALL`, so the hand's visual state machine is driven by the same locomotion state names
the audio/event system uses. This is the hook vocabulary the feather runtime should consume.

## Inferred (not yet proven)

- The right/left hand pair is a **skinned mesh** using the 16-bone `entity` path. The
  material/texture pairing and the `hand1fract`/`hand2fract` variants are consistent with a
  mesh split into bone/nerve/vein/fracture layers, but no mesh record has been identified.
- `hand2`/`hand2left` are texture atlases, not mesh names. They appear through the type-1
  resolver in `0x59e90`, while every hand material goes through the type-2 resolver.
- The iridescence is produced by the `entity` material family (reflection/refraction,
  `tex_reflect`, `tex_edgespec`, `spherical_proj`, `accum_time`, `iri`), not by the
  simple `hand` material. Reusing `entity_hand`/`entity_hand_vs` is the correct way to
  preserve it.

## Not yet known (why Milestone B is blocked)

1. **No hand mesh record has been confirmed.** The live probe recovers 860 records and
   names only a subset; none of the recovered records is a hand mesh. `hand2left` was
   text-scored to a record but the direct CRC seed did not decrypt it, so that assignment is
   not accepted.
2. **The renderer's draw calls are invisible.** `KERNEL32!GetProcAddress`,
   `opengl32!wglGetProcAddress` and every direct `opengl32` import were patched; the engine
   requests only four `wgl*` names and no `gl*` names. Swap-time readback works, per-draw
   tracing does not. There is no verified way to observe the hand's vertex buffer, index
   count, material binding or bone matrices at draw time.
3. **The geometry may be procedurally generated.** The hand material's vertex shader has
   no skinning, and the hand textures are loaded separately from the fracture materials.
   A procedural ribbon/bone-strip is consistent with the screenshots (segmented, tapered,
   faceted arm) but has not been distinguished from a static skinned mesh.

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

## Asset authoring status (Milestone C, partial)

The independently authored replacement is generated by
`tools/assets/build_berdly_arm.py`; no game mesh, texture or shader bytes are used. The
current artifact is an **art/rig target, not an in-game replacement**.

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

**Known integration mismatch:** the authoring rig has 26 bones but the verified
`entity` path supplies at most **16** `bone_transforms` matrices. The mesh metadata now
marks `runtime_skinning_status: pending-16-bone-bake`; a 16-bone collapse (merging the
second finger/claw joints and consolidating feather rows) must be baked and validated before
any draw-path integration. Per-vertex influence count alone is not sufficient.

Visual iteration was inspected from the generated renders in
`assets/berdly-arm/renders/`: the arm reads as a deep-blue, faceted, clawed avian limb with
layered vanes, visible iridescent blue/violet highlights and dark hooked claws. The framing
is still an art-direction approximation of the game camera, and none of this is game-render
verification. The current first-person render still has substantial empty frame around the
arm and the vanes read more like sharp plates than fine barbs; both are open art issues.

## Next experiments, in order

1. **Full live record dump with names.** Extend the probe to report every record and the
   complete key arrays (not the 400-record cap) and pair records by recovered key class.
   Then locate type-1 records whose names contain `hand`/`fingershell` and decrypt them.
   This is offline and safe.
2. **Locate the hand entity's vtable and update.** The constructor `0x59e90` is referenced
   from data at `0x1403e0f28`, `0x14038bf20`, `0x140364ac0`; walk that vtable to the draw
   and update methods, then decompile the method that writes `bone_transforms`. That yields
   the bone count, rest pose and gameplay inputs without guessing.
3. **Per-draw tracing through the Mesa dispatch.** Interpose `glXGetProcAddress` in a
   Linux-side `LD_PRELOAD` probe, or hook the engine's internal GL dispatch table once (2)
   identifies it. Capture `glDrawElements`, bound VAO/VBO, attribute pointers, current
   program and `bone_transforms` for the hand draw. This is the decisive Milestone A/B
   experiment; it does not require modifying the installation.
4. **Then, and only then, choose the replacement mechanism**: a same-length pack slice
   substitution if the hand is a mesh asset; a verified draw/buffer substitution if it is
   procedural. Both must be reversible and gated on the build fingerprint.

## Reproducible tools

```bash
# enumerate the material library and verify the CRC key classes
python3 tools/re/name_records.py --game "$GAME" --json /tmp/assets.json

# decrypt one material (no bytes written by default)
python3 tools/re/pack_crypt.py --exe "$GAME/hyperdemon.exe" --pack "$GAME/asset/asset" \
    --name hand --offset 306587727 --size 1197 --hex

# the replacement mesh/rig used for the visual target
blender -b --python tools/assets/build_berdly_arm.py
```

The asset generator is independent of any game content: every mesh, feather, bone, weight
and material is authored in the script.
