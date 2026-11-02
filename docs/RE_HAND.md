# The first-person hand: rendering and animation pipeline

Status: **Milestone A complete; Milestone B in progress.** The material/shader path, the
skinning path and the gameplay-facing material set are identified from the shipped pack and
the executable. The live hand draw is now located and proved by suppressing it and by
substituting its vertex/index buffers. The hand is a 238-vertex, 72-byte-stride mesh behind a
two-pass 1,188-index draw. Its six attributes carry position, normal, `uv0`, `uv1`, tangent
and bitangent; **no skin indices or weights are in this buffer**, so the mesh is posed before
upload rather than skinned by the draw. A production replacement is not yet integrated: the
first test mesh was not correctly placed and rendered only as a fragment. The persistent
dispatch hook is now written and proved live as `example.hand-hook`. This document separates
what was measured from what is inferred, and names the next experiment.

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
| Index type | `GL_UNSIGNED_INT` (5125, read from the call setup) |
| Attribute buffer | 41 (all six enabled attributes) |
| Element buffer | 42 |
| Attribute buffer size | 17,136 bytes |
| Element buffer size | 4,752 bytes (1,188 `uint32` indices) |
| Vertex stride | 72 bytes |
| Vertex count | 238 (`17136 / 72`) |
| Enabled attributes | six: 3, 3, 2, 4, 3, 3 floats, all stride 72, all in buffer 41 |
| Attribute offsets | 0, 12, 24, 32, 48, 60 bytes |

The first three floats of each 72-byte record are object-space position; the next three are
normal. The remaining fields carry tangent/UV and the skinned-entity data.The 4,752-byte index buffer is exactly one run of 1,188 `uint32` indices
(4,752 = 1,188 x 4), so the two adjacent draws are two passes over one index
run, not two 16-bit runs or two different hands.

## Verified live: the persistent dispatch hook

`examples/hand_hook.c` (`example.hand-hook`) installs the production hook from a normal mod:

- it gates on `build_matches()`, reads the 8-byte slot at RVA `0x3d9ac8` (the live value was
  `0x7bee1450`, the `glDrawElements` entry), writes its shim there through ABI 6
  `image_write`, and reads the slot back to confirm the write;
- the shim forwards every unrelated draw to the saved pointer and matches the hand by
  `mode == GL_TRIANGLES`, `count == 1188` and index type `GL_UNSIGNED_INT` or
  `GL_UNSIGNED_SHORT`;
- in `suppress` mode it sets the count to zero for those two passes; `pass` mode forwards
  them unchanged. The mode is read from `<state_dir>/hand-mode.txt` at initialization;
- `shutdown` calls `image_restore`, and the host also restores every patch at process exit.

The shim performs no host logging and no GL queries. An earlier version logged the first
interception through the host API from the render thread, and the game died on the first
gameplay hand draw; the shim is now pure (count, optionally suppress, forward).

**Live result.** On the fingerprinted build (8/8 anchors, base `0x140000000`) the game logged
`patched 8 byte(s) at rva 3d9ac8` and `dispatch hook installed ... (original=0x7bee1450
mode=suppress)`, initialized successfully, and rendered gameplay with the world and birds but
**no first-person arm**. The captured frame is 1920x1080, sha256
`cbc0acecd0fb0e516e989bd08a9e719f17941ddbef40135343f7ada2b901a71d`; it is not committed
because it contains game content. The process was alive at capture.

**Stability control.** The suppress run exited a few seconds after its capture, with no
`hyprlodr-crash.json` and no shutdown log. A `pass-through` run with the same hook and mod set
was still alive more than 20 s after launch, and its capture shows the **original golden arm**
rendering normally (sha256
`b565488ced237361c6716b29e53ed921381f1fb3fa7bcca9cdaf9f36474d5e33`). The hook's presence
alone is therefore not the cause, and pass-through is a working fallback that preserves the
stock arm. Sustained suppression (zero count every frame) is still unproven and is not a
shippable default. Neither run reached the loader's shutdown, so the in-memory patch simply
died with the process; `image_restore` was not exercised in-game.

**Correction that mattered.** The element type is `GL_UNSIGNED_INT` (5125), not
`GL_UNSIGNED_SHORT`; the 4,752-byte buffer is 1,188 `uint32` indices. The original
`GL_UNSIGNED_SHORT` test cube therefore fed a 72-byte index buffer to a draw that consumed
32-bit indices, which is what crashed the earlier live session.

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
3. **Persistent hook.** Implemented and live-verified (previous section). The remaining work
   is stability: run `pass` mode for the same duration to separate suppression from the
   hook's mere presence, and only then make suppression a default.
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

**Authoring correction (Blender preview only).** The feather builder unpacked
`frame_for()`'s `(right, up, forward)` basis as `(forward, up, right)`, so the
requested feather directions were replaced by their perpendicular axis. The
unpack now matches the helper, and the lateral rows point mostly along the
forearm with modest left/right splay. The regenerated preview reads as a compact
layered feather coat instead of a broad side fan; its game-space framing and
gold iridescence still need a live capture.

Selected feather rachises now use the warm iridescent material while the vane
bodies stay blue, making the gold interference readable as a narrow accent at
first-person scale without broadening the gold coverage.

The Blender material preview now feeds the view-dependent color ramp from the
mesh's geometry normal. Generated texture coordinates remain on the fine-grain
noise input; they are not used as a surface normal.

The optional authored preview now uses a wider gold band in its blue-to-cyan
prism ramp, a brighter warm-metal base, and gold on about one third of selected
feather vanes. This makes the gold read alongside the blue in the offline
first-person render. It changes only the Blender-authored model materials; the
native animated `palette` path and its in-game gold selection are unchanged.

Short overlapping covert feathers now continue along the exposed forearm in
seven closer-spaced rows, reaching nearer to the wrist cuff. Each card is
narrower than before, keeping the lower arm blue and feathered while preserving
its narrow silhouette and leaving the gold interference to selected rachises,
palette and game reflection path.

The wrist-anchored primary, up and alula rows now sweep back along the forearm,
and the primary row has narrower cards without a duplicated covert layer. This
clears the generated palm/finger silhouette; the change is limited to the
optional authored mesh and does not replace or retarget the game's native hand
animation.

The authored arm's upper and lateral feather rows are now narrower, and the
forearm coverts are shorter. This reduces the large plate-like overlaps around
the wrist and palm in the generated first-person previews while retaining the
same outer arm silhouette. The runtime bake and hand-mesh conversion were
regenerated; the resulting 3.32 MB mesh remains below the hook's 4 MiB asset
limit. This is an authored-model improvement only; the live default remains the
native animated hand in palette mode.

Visual iteration was inspected from the generated renders in
`assets/berdly-arm/renders/`: the arm reads as a deep-blue, faceted, clawed avian limb with
layered vanes, visible iridescent blue/violet highlights and dark hooked claws. Feather cards
now use additional cross-sections and mild edge scalloping to suggest barbs while keeping the
same outer silhouette. The framing is still an art-direction approximation of the game camera,
and none of this is game-render verification.

## Geometry-substitution stage (verified visible)

`example.hand-hook` 0.3.0's `cube` mode creates a private VAO/VBO/IBO at the first owned draw
and binds it for both hand passes. Attribute locations were read live as **0, 1, 2, 3, 4**
(`in_position`, `in_normal`, `in_uv0`, `in_uv1`, `in_tangent`) with program 89, so the measured
layout maps directly. The engine's buffers are never read or written.

The first attempts drew nothing for a concrete reason: `glBufferSubData` on freshly generated,
zero-sized buffers raises `GL_INVALID_VALUE` (1281) and leaves the VBO/IBO empty. Using
`glBufferData` to create the stores fixed it; the GL error after prepare and after the draw is
now 0. The cube's first UVs also sampled a transparent atlas region; a UV copied from a live
hand vertex (`0.0643, 0.7835`) made it visible.

Live debug from the running process: one prepare call, all eight GL entry points resolved
(bitmask 255), geometry ready, **10,768 of 10,768 owned draws substituted**. The screenshot shows
a large textured slab filling the lower frame, with the original arm gone and the world and
birds still rendering (sha256 in `docs/evidence/2026-11-01-hand-cube.json`). This is the first
visible geometry replacement.

The cube is deliberately crude and currently oversized and too close to the camera. Its scale
and pivot must be fitted to the hand's model space before the authored arm mesh can replace it.
Depth test and culling are disabled only for this draw and restored immediately after.

## Authored arm stage (verified visible)

`example.hand-hook` 0.4.0's `arm` mode reads `hand-mesh.bin` from the mod state
directory during **mod initialization**, never inside the engine's draw call,
validates the `HYPRMESH` header (version, stride 72, counts and exact file size),
and uploads the vertices and indices into the same private VAO layout as the
cube. No game buffer is touched.

`tools/assets/convert_glb_to_hand_mesh.py` converts `berdly_arm.runtime16.glb`
into that binary: positions in hand space, authored normals and UVs, and u32
indices. The converter packs the source's five material slots into separate
palette columns (blue, light blue, warm cream/gold, dark claw blue and
warm metallic gold on the iridescent feather accents). The hook binds that
palette only to each active hand program's `tex_grad` unit. It leaves
`tex_diffuse` bound to the game's screen-space hand blend and preserves
reflection and edge-specular inputs for
the HYPER DEMON shimmer. The live-verified
orientation is a 180-degree X remap. Based on the recorded oversized, centered
capture and the stock mesh's measured span, the current asset fit is reduced to
scale 0.32 and shifted right/down to offset (-0.0440, -0.1442, -0.1000).
Its resulting bounds are generated in `hand_mesh.meta.json`; this latest fit
has not yet been checked in gameplay.

Live counters: 17,322 vertices and 25,620 indices (8,540 triangles, 1,349,712
bytes) uploaded; **12,112 of 12,112 owned draws substituted**; GL error 0 after
prepare and after draw; the game stayed alive. The screenshot shows the authored
winged claw silhouette, with its palm and spread digits, lit by the game's hand
material (hash in `docs/evidence/2026-11-01-hand-arm.json`). The stock arm is
gone.

The palette path preserves the game's screen blend and reflection inputs. The
first version supplied a 256x256 five-column texture authored for the
replacement mesh, which assumed the native hand used the same UV layout. Live
texture dumps show the game uses 64x64 gradient ramps; version 0.7.0 reads the
active `tex_grad` pixels and keeps their dimensions and alpha. Version 0.7.1
shifts the native gradient to blue while reserving only its brightest warm
texels as gold accents. Version 0.7.2 builds a separate recolored copy for
each hand program, preserving distinct source ramps across both passes.
Version 0.7.3 recognizes saturated yellow-gold as well as orange-gold while
excluding neutral white highlights from the preserved accent range. Version
0.7.4 keeps only the brightest warm band, so the native gold ramp stays a
small accent over the blue recolor. Version 0.7.5 modestly lifts the blue and
cyan channels in the remaining ramp to keep the hand legible at first-person
scale. Version 0.7.6 blends the gold tip into blue over a short luminance range
to avoid a hard boundary in warm gradients. Version 0.7.7 starts the warm-to-
gold blend at luma 190 and gives it a wider transition, retaining more of the
native gold ramp while the hue gate excludes neutral white highlights. Version
0.7.8 lifts the blue/cyan channels and broadens the warm-to-gold transition
again, keeping the red channel restrained so the hand stays blue outside the
warm ramps. The game's original reflection samplers still provide animated
iridescence. Version 0.7.8 was compared offline against the captured game
gradients. Its blue formula could exceed 255 at the brightest texels and wrap
to green when converted to 8-bit color. Version 0.7.9 clamps the recolored
channels before blending, preventing that overflow while retaining the brighter
blue/gold mapping. Version 0.7.10 lowers green while keeping blue bright,
shifting the blue/cyan ramps toward cobalt without broadening the gold
selection. The overflow comparison is `.tmp/palette-079-byte-clamp.png`, and the
blue-balance comparison is `.tmp/palette-0710-blue-balance.png`. Live gameplay
verification is still required to confirm the resulting color balance.
Version 0.7.11 starts the warm-to-gold transition earlier and spreads it over
a wider luminance band. The offline comparison in
`.tmp/palette-gold-band-candidate.png` shows the extra gold shoulder on warm
ramps while leaving the cyan ramp blue. It preserves the native geometry,
animation and reflection path; a gameplay capture is still needed to verify
the visual balance on the hand.
Version 0.7.12 eases the same transition toward the original warm ramp, so gold
appears sooner and the midrange spends less time in a muted RGB blend. The
comparison is `.tmp/palette-gold-ease-candidate.png`; blue and cyan-only ramps
are unchanged. Live gameplay is still needed to judge the result on the hand.
Version 0.7.13 uses a stronger cubic ease-out over that same band. The preview
`.tmp/palette-gold-easing-curves.png` shows the warm ramp reach gold sooner
while retaining the blue upper half; the cyan-only ramp remains unchanged.
Gameplay verification is still required.

The hook records palette setup in `hypr_hand_debug[35]` (1 ready, 2 missing GL
entry points, 3 no `tex_grad`, 4 invalid unit, 5 shared unit, 6 texture allocation
failure, 7 no active shader, 8 no texture bound to `tex_grad`, 9 source gradient
exceeds the safe 256x256 readback buffer, 10 more programs than the two-entry
cache supports). Live verification of the sampler
change and adjusted framing is still required.

### Native animation palette mode

The static replacement visibly drops the game's hand pose animation and
proportions. Hook version 0.7.0 adds `hand-mode.txt` value `palette`: it keeps
the original hand VAO, posed vertices, index buffer and both draw passes, and
temporarily binds a recolored copy of the active game gradient only when
`tex_grad` has a dedicated unit. The original `tex_diffuse` screen blend and
reflection/specular samplers remain bound. If the shader has no safe gradient unit, the draw is forwarded
unchanged. This restores the native hand's shape and animation; it does not add
physical feathers. The authored `arm` replacement remains available for visual
comparison. The first `palette` implementation never resolved GL functions
because only replacement modes initialized them; this was fixed by resolving
them during the first native hand draw. The pre-fix gameplay capture therefore
shows the stock gold hand. The 2026-11-02 fresh Proton launch resolved hook
version 0.7.3, installed the dispatch hook, and initialized in `palette` mode.
Xephyr delivered raw mouse and keyboard events to the title window, but both
the menu clicks and the existing `/dev/uinput` helper left the game on its
title screen. The blue balance is therefore still not visually verified in
gameplay. On the next live check, `hypr_hand_debug[4]` counts matched hand draws, `[34]` records
the active program, `[35]` reports palette setup status, `[36]` counts native
programs without a safe gradient unit, and `[32]` counts draws that used the
replacement gradient texture.

The 2026-11-02 0.7.3 launch again confirmed mod discovery, initialization,
dispatch-hook installation, and `palette` mode. The game stayed at the title
screen and no usable gameplay capture or gameplay hand draw was obtained, so this
confirms loader integration only, not the revised gold selection or in-game
appearance. The revised 0.7.4 gold cutoff was inspected against the captured
64x64 gradient ramps offline; it has not yet been checked in a live hand draw.
The subsequent 0.7.4 Proton launch confirmed mod discovery, initialization,
hook installation, and `palette` mode, then was stopped at the title screen.
Version 0.7.5's brighter blue mapping was compared offline against the same
captured gradients; live color balance still needs a gameplay capture.
The 0.7.5 Proton launch confirmed mod discovery, initialization, hook
installation, and `palette` mode. It was stopped at the title screen, so the
native hand draw did not exercise the updated palette in this launch.
The 0.7.6 transition was previewed offline against the captured gradients;
gameplay color still needs a live hand draw.
The 0.7.6 Proton launch confirmed mod discovery, initialization, dispatch-hook
installation, and `palette` mode, then was stopped at the title screen without
reaching a native hand draw.
The 0.7.7 Proton launch likewise resolved and initialized version 0.7.7, patched
the dispatch slot, and reported `mode=palette`; the game rendered its animated
title screen and remained alive. Mouse hover/click and Return did not enter Play,
so the native hand draw still did not exercise this palette update. The capture
is `.tmp/hand-hook-077-launch.png`; this confirms loader integration only, not
in-game blue/gold appearance.
The 0.7.8 cross-build was installed and launched through Proton. HYPRLODR
resolved and initialized version 0.7.8, patched the same dispatch slot, and
reported `mode=palette`; the game again reached the title screen but not a hand
draw. The capture is `.tmp/hand-hook-078-launch.png`. This verifies startup for
the new palette code, while its color balance remains an offline preview only.
The 0.7.9 build was then loaded the same way: the loader resolved and
initialized the version, patched the dispatch slot, and reported `mode=palette`.
The game reached its animated title screen and was stopped there; no hand draw
was reached. The capture is `.tmp/hand-hook-079-launch.png`. The overflow
correction is build- and startup-verified, but still needs a gameplay frame for
visual verification.
The 0.7.10 build was launched through Proton and likewise resolved,
initialized, and installed the dispatch hook in `palette` mode. It reached the
animated title screen, where the run was stopped without a gameplay hand draw.
The capture is `.tmp/hand-hook-0710-launch.png`; the blue-channel balance remains
offline-previewed rather than gameplay-verified.
The 0.7.11 build was installed and launched through Steam on the 1920x1080
Xvfb display. HYPRLODR matched all eight build anchors, resolved and initialized
version 0.7.11, installed the dispatch hook, and reported `mode=palette`. The
game reached its animated title screen, but XTest click and Return input did not
enter gameplay. The capture is `.tmp/handmod-0711-launch.png`; this verifies
loader startup only, not the updated gold band's appearance on the hand.
The 0.7.12 Steam launch also resolved and initialized the mod, matched all
eight build anchors, and installed the dispatch hook in `palette` mode. The
title screen remained unresponsive to XTest click and Return input, so the
updated easing has not been visually verified on the animated hand. The capture
is `.tmp/handmod-0712-after-input.png`.
The 0.7.13 Steam launch likewise matched all eight anchors, initialized the
mod and installed the hook in `palette` mode. XTest click and Return still did
not enter gameplay; `.tmp/handmod-0713-after-input.png` shows the title screen,
not the hand. The cubic easing is build- and startup-verified only.

Version 0.7.14 lowers the green contribution in cool tex_grad ramps from 65%
to 48% of luminance, making the blue body read more cobalt while leaving the
warm-pixel gold selection and original reflection samplers unchanged. The
offline comparison against captured ramps is `.tmp/palette-0714-cobalt-candidate.png`.
The fresh Proton launch resolved 0.7.14, matched the build anchors, and installed
the hook in `palette` mode. It reached the title screen, but the virtual input
device still could not enter gameplay, so the new blue balance has not been
visually verified on a hand draw.

Version 0.7.15 lowers the warm-ramp gold onset from luma 145 to 128 and remaps
the easing span so selected yellow highlights become gold sooner. The captured
texture comparison is `.tmp/palette-0715-gold-transition-candidate.png`; the
cobalt ramp is unchanged. A fresh Steam launch resolved 0.7.15, matched all
eight build anchors, and installed the dispatch hook in `palette` mode. The
capture `.tmp/hand-hook-0715-frame-480.png` is the title screen; no hand draw
occurred, so startup is verified while gameplay color remains unverified.

Version 0.7.16 copies the source `tex_grad` magnification and wrap parameters
to its recolored texture, and keeps the source minification filter when it does
not require mipmaps. Since the replacement only contains level 0, mipmap
filters fall back to linear. The Windows build and a Steam startup both
succeeded; the hook resolved 0.7.16, matched all eight anchors and installed in
`palette` mode. The title-screen limitation still prevented a hand draw, so
this runtime sampling adjustment has not been visually exercised.

Version 0.7.17 normalizes OpenGL pixel-pack state around `glGetTexImage` and
temporarily unbinds any pixel-pack buffer before reading the source gradient.
It restores the prior alignment, row length, skipped rows/pixels, PBO binding,
and active texture afterward. This protects the copied ramp from unrelated
readback state. A fresh Steam launch resolved 0.7.17, matched the build, and
installed the dispatch hook in `palette` mode. X11 click/key input and a
Windows `SendInput` Enter/click both left the game at its title menu, so no
hand draw exercised the updated readback path. The game was stopped after the
capture; the installed DLL remains 0.7.17, with the 0.7.16 DLL backed up under
`.tmp/` for rollback.

### Orientation fix (verified live)

The first uploads kept the glTF accessor's coordinates unchanged. The exporter
writes the authored mesh on a **Z-long** axis with the claws at **-Z**; in the
live hand pass **+Z is screen-up**, so those uploads put the claws at the near/
bottom end and made the hand face the player.

Three controlled in-game tests established the mapping:

| Remap | Result |
|---|---|
| none | vertical arm, claws at the bottom/near end (wrong) |
| `R_x(90)`, `(x, y, z) -> (x, -z, y)` | horizontal arm across the bottom (rejected) |
| `R_x(180)`, `(x, y, z) -> (x, -y, -z)` | vertical arm, claws at the far/top end, wrist through the bottom edge (accepted) |

The accepted transform also lowers the mesh (offset Z -0.080) so the wrist
exits the frame instead of floating. Normals are rotated with the same matrix.
Live counters: **12,096 of 12,096** hand draws substituted, 17,322 vertices and
25,620 indices, GL error 0, game alive. The screenshot shows the authored arm
entering from the bottom with the claws pointing into the gameplay area; it is
recorded by hash in `docs/evidence/2026-11-01-hand-orientation.json` (the frame
itself is not committed because it contains game content).

The transform is applied to static geometry, so it inherits the game's hand
world/view transform each frame and stays camera-locked. It is still bind-pose
geometry: the seven baked actions and feather springs are not applied yet.

## Original-arm extraction and feather overlay (2026-11-02)

**The original arm is recoverable as posed geometry, not as a source rig.** A local live dump
(`/tmp/hand-vbo.bin`, 17,136 bytes; `/tmp/hand-ibo.bin`, 4,752 bytes; neither committed) gives
238 vertices at the measured 72-byte stride and 1,188 u32 indices (396 triangles). All 238
vertices are used by one connected component. The two 594-index halves overlap 197 vertices, so
they are material/part groupings, not two hands.

The record layout is confirmed from the bytes: position at 0, normal at 12, `uv0` at 24, a
four-float attribute at 32 (`x` varies 0.0051..0.0668, `y` is constant 0.0922, `z/w` zero),
tangent at 48 and bitangent at 60. The four-float attribute is **UV/mask data, not bone indices
or weights**; the mesh is posed before upload. The undeformed source mesh and rig are therefore
not recoverable from the draw buffer alone. Recovering them would require the CPU skinning path
or the original asset, neither of which this buffer contains.

`tools/assets/import_hand_vbo.py` reconstructs the captured mesh in Blender with positions,
per-vertex normals, both UV sets and the two index groups. It writes only to the ignored
`assets/hand-extract/` directory; the dump and the `.blend` are never committed.
`tools/assets/build_feather_overlay.py` then authors 43 feather cards on that surface. Every
feather vertex is bound to its four nearest original vertices (weight-sum error 2.2e-16), and the
tool exports a 64,360-byte `feather_overlay.bin` (860 vertices, 774 triangles) using a small
binary format with positions, normals, UVs and the four bind indices/weights.

`example.hand-hook` 0.9.0's `overlay` mode keeps the native arm draw and animation, reads the
live 238-vertex VBO, deforms the feather vertices by the same weighted per-vertex deltas and
draws the result after the second hand pass with the authored blue/gold palette. A live lobby
run loaded the feather mesh and bind pose, prepared the private VAO, and recorded **15,364
overlay updates and draws with GL error 0**; the native arm stayed visible. **Visual result:**
the overlay draws, but the captured frame does not show separable feather layers — most cards
still read as part of the gold arm silhouette. This is a placement/occlusion result, not a draw
failure. The next check is an overlay-only diagnostic and a bind capture from the same live
pose; do not treat the counter as visual acceptance.

## Next experiments, in order

1. **Substitute geometry through the verified hook.** The shim now owns the two hand draws.
   Bind our own VBO/IBO (or write the game's buffers and restore immediately) for a solid test
   mesh with the corrected `GL_UNSIGNED_INT` indices, prove it is visible and reversible, and
   only then upload the authored arm. Keep all host calls out of the shim.
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
