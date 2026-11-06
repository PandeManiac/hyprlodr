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
`tools/assets/build_feather_overlay.py` authors 24 feather cards on that surface and appends a
blue shell made from the extracted arm itself. The shell is the 238-vertex captured surface
offset 0.0025 outward along its normals; the whole overlay is 718 vertices / 2,484 indices
(828 triangles) / 55,920 bytes. Every shell and feather vertex is bound to its four nearest
original vertices (weight-sum error 2.2e-16), so the shell and cards follow the same live
per-vertex deltas. The first 43-card layout was a dense 64,360-byte shell and read as a blob;
the current layout uses four shorter covert rows, five swept primaries and two hand coverts.

`example.hand-hook` 0.9.2's `overlay` mode keeps the native arm draw and animation, reads the
live 238-vertex VBO, deforms the shell and feather vertices by the same weighted per-vertex
deltas and draws the result. **The pass order was the integration bug:** drawing after the second
hand pass used the screen-blend program, so the shell was invisible or sampled the wrong state.
Drawing after the first color pass made it visible in the `overlay-only` diagnostic: **9,081
overlay updates/draws**, bind loaded, palette created, GL error 0. The palette is bound across
every texture unit for the feather draw instead of only the single `tex_diffuse`/`tex_grad`
sampler.

**Live gameplay verification:** a second instance was launched directly through Proton on the
existing Xvfb `:99` display at 1920x1080, leaving the unrelated Gunfire Reborn session on `:0`
untouched. XTest and Win32 `PostMessage` are ignored by the game's input path; a Win32 helper
running in the same Proton prefix using `SetCursorPos` + `mouse_event`/`keybd_event` reached the
title menu and entered gameplay. The `overlay` capture shows the native animated gold arm with
the blue shell covering the forearm and the blue/white feather crest at the wrist, with gold
highlights. The local capture is `.tmp/99-shell.png` (not committed). This is the first combined
live blue/gold shell capture.

**Still not accepted:** the feather cards cluster as a cape at the wrist rather than following
the forearm, the shell's material reads mostly blue/white instead of reproducing the original
gold interference, and the modeled 41,482-vertex Berdly mesh / 16-bone rig is not yet bound to
the live buffer. The shell is a copy of the original arm, not yet the authored Berdly arm.

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

## 0.9.3 authored overlay capacity and depth (2026-11-02)

`example.hand-hook` 0.9.3 raises `FEATHER_MAX_VERTICES`/`FEATHER_MAX_INDICES` to 65,536 /
131,072. The generated 41,482-vertex / 62,436-index authored mesh therefore loads through both
`overlay` and `arm` modes; the Windows build and all 7 CTest tests pass.

The overlay now preserves the authored normal rather than copying the nearest native normal. For
each overlay vertex it takes the weighted native bind normal and current normal, computes the
shortest-arc rotation between them, and applies it to the authored normal. The shell and cards
still follow the live native per-vertex deltas.

Depth behavior is mode-specific. Combined `overlay` mode draws after the native first color pass
with depth testing disabled, because the authored surface is not coplanar with the native gold
arm. `overlay-only` suppresses the native draws and leaves depth testing enabled so the authored
layers self-occlude instead of stacking into a feather mass.

**Live verification and limits:** `.tmp/99-shell-093.png` verifies the 0.9.3 shell fallback in
gameplay. The authored-mesh captures `.tmp/99-authored-normals.png`, `.tmp/99-auth-depth.png`
and `.tmp/99-arm-41k.png` confirm loading and visibility but are not accepted as the final arm.
The overlay still presents a low wrist/feather mass without a readable authored hand, and direct
`arm` mode is static, centered and gold because it does not inherit the native first-person
transform. The shell remains the verified fallback; authored fit, hand alignment and the
animated gold iridescence are still blockers.

## Authored arm axis flip and reproducible binder (2026-11-02)

The authored overlay's long axis was inverted in the first native-space fit. After the
nearest-native bind, native Y is now negated (`--no-flip-y` disables it for diagnostics),
which moves the authored claws to the top of the frame and the forearm/feathers below.
The live captures are `.tmp/99-flip.png` (authored-only) and `.tmp/99-flip-overlay.png`
(native plus authored). They show the authored claws and layered feathers following the
live hand buffer.

`tools/assets/build_custom_overlay.py` makes the derived overlay reproducible from the
authored `hand_mesh.bin` and the local 238-vertex bind dump. It applies the verified
centre/scale/outward-bias transform, binds each vertex to the four nearest native
vertices with `1/(distance + 0.004)` weights, and writes the compact `HYPRFEAT` runtime
record. Its pre-flip output is byte-identical to the captured overlay; the flipped output
differs only in normal low bits (5.96e-8). Metrics: 41,482 vertices, 62,436 indices,
2,904,624 bytes, 162 used native vertices, normalized weights (2.2e-16 error).

A gold-tip palette variant was tested in `.tmp/99-gold-tip.png` and rejected because the
arm washed pale ice/white. The committed blue/white palette is restored. The authored
material still does not reproduce HYPER DEMON's animated gold iridescence, and combined
mode can show native gold along the hand edge; material separation and final fit remain
the next work.

## Passive hand geometry capture (0.9.5)

`example.hand-hook` 0.9.5 adds an opt-in `capture` mode. It uses the already verified draw
dispatch slot and accepts exactly the original `GL_TRIANGLES` / 1,188-index and enlarged
1,191-index unsigned-int signatures, while forwarding original draw arguments unchanged.
Before capturing the first matching draw, it verifies all six enabled attributes against
the measured 72-byte layout and checks both buffer extents. It reads the logical VBO/IBO
sizes (17,136/4,752 bytes original; 17,208/4,764 enlarged) into fixed storage. It restores
the prior `GL_ARRAY_BUFFER` binding; it never rebinds the element buffer or modifies either
buffer. A single
`glGetBufferSubData` readback can stall the render thread, so capture mode is for a
controlled diagnostic run only.

The callback makes no allocation, formats no log messages and does no filesystem I/O.
It signals a small writer thread after one successful snapshot. The writer emits a
104-byte `HYPRCAP1` header followed by the raw vertex and index bytes. The header stores
draw ordinal, a pair-derived frame estimate, monotonic milliseconds since boot, draw
signature, buffer IDs, index offset and six attribute offsets. The frame estimate uses
the previously measured pair of hand passes; it is not an independent swap/frame count.

To opt in, `<state_dir>/hand-mode.txt` contains two lines: `capture`, then an absolute
output path containing `.tmp`. The capture is therefore explicit and stays out of the
game installation. The reusable analyzer is `tools/re/hand_capture.py`:

```bash
python3 tools/re/hand_capture.py .tmp/hand-capture/runtime/hand2.capture \
  --original .tmp/analysis/hand2-original-expected.bin \
  --modified .tmp/analysis/hand2-modified-expected.bin \
  --json .tmp/hand-capture/runtime/comparison.json
```

It checks the capture structure and hashes, then reports exact byte equality,
index-topology correspondence, exact UV0 / offset-32 attribute matches, unique
vertex reordering where the keys are unambiguous, a global affine fit with position
residuals, and normal direction error after the fitted transform's inverse transpose.
It does not label counts alone as an identity match. UV0 and the
bytes at offsets 32–47 are used as matching keys without assigning them new semantics.
Ambiguous keys remain ambiguous; non-rigid CPU posing can produce a poor affine fit.

**Offline validation:** synthetic captures cover exact bytes, a transformed mesh,
vertex reordering, duplicate-key ambiguity, truncated data and out-of-range indices.
The focused Python tests and the Windows cross-build pass. Proprietary captures are
never committed.

**Runtime result (2026-11-03): no hand geometry was captured.** The first attempt used
`delay_ms: 0`; the verified build loaded, but the dispatch slot was still null when the
mod initialized, so it declined to patch. A second attempt with `delay_ms: 2500`
installed the capture hook at RVA `0x3d9ac8` and presented renderer frames. However, the
first `_Fiopen` of `asset/asset` at 415 ms reported `redirected: false`; the redirect
was registered and published later. No `hand2.capture` file appeared, and the process
exited before gameplay input could be attempted. The installed proxy hash was
`8e0a069370f76de7084c39f23360311715f18677236d0e767c9af3f7992dbe64`; its binary did not
contain the `redirect_wait` event string used by the checked-in source, and the runtime
log contained no wait event. This supports a stale installed proxy as the redirect
blocker, but does not establish why the game exited. This run is not evidence that the
modified `hand2` was loaded or unused.

All 23 experiment-touched installation paths were restored and checked against their
pre-run hashes or symlink targets. The original archive hash remains
`2ce9dabb3251cb8d7994e2bda74056d344c06bf55b0d47b8645029b4687f99c7`; the executable,
proxy, and bootstrap configuration also match their recorded pre-run hashes. Run logs,
frame images and restore records remain in ignored `.tmp/hand-capture/`.

## Readiness retry and reproducible native deployment

The 0.9.5 capture build no longer assumes that the engine's dispatch slot is ready
during mod initialization. It starts a bounded worker (10 ms polling, 30 second
limit) which reads only the fingerprint-verified RVA, requires a committed
executable target via `VirtualQuery`, and re-reads the slot immediately before
the write. Installation, verification, timeout and shutdown have explicit states;
shutdown stops and joins the worker before restoring image patches. The render
callback still forwards the original call unchanged in `capture` mode. Archive
redirect publication remains the loader's separate initialization step.

`tools/build_windows.sh` writes `hyprlodr-proxy-build.json` alongside the proxy,
including the source revision, clean-tree bit, artifact SHA-256 and versioned
capabilities. The proxy reports the same identity in its runtime attach record.
`tools/hand_capture_deploy.py preflight` checks that identity against the current
revision, the known game executable hash, both required mods, the redirected
archive's unchanged regions, and the decrypted edited tag-1 `hand2` payload.
`deploy` journals and backs up every file it stages below `.tmp/hand-capture/`;
`restore` puts experiment-owned files back and preserves runtime logs/captures in
that persistent directory. It never writes the installed archive or executable.

The readiness state helper has native focused coverage for a null/delayed slot,
non-executable targets, a successful install, write/verification failures, and
shutdown. The deployment comparison tests cover archive size and differences
outside the designated record. Runtime verification must separately confirm the
proxy identity, `redirected: true` on the first archive open, dispatch install,
gameplay hand draws, and the resulting `hand2.capture`; none of these stages
should be inferred from a title screen or an empty capture file.

**Controlled runtime result (2026-11-04): deployment, archive redirect and hook
readiness succeeded; gameplay geometry remains unobserved.** The first staged run
used commit `375c0c9`. Its runtime attach record showed the correct executable and
8/8 anchors, and the redirect mod began initialization, but `example.hand-hook`
stopped during initialization after its `state_dir` callback. A Wine fixture then
reported `KERNEL32.InterlockedExchange` as unimplemented. That build imported the
Interlocked functions; this was a plausible explanation for the stalled capture
mod, although the game emitted no crash record proving causation. Commit `a9abf3b`
replaced those imports with x64 lock-free atomics. The actual mod fixture then
initialized in capture mode, waited through five null dispatch observations,
installed against an executable target on observation six, and restored the slot
on shutdown.

The follow-up used the clean `a9abf3b` build. The runtime attach record reported
that exact revision and capabilities `redirect-wait-v1,build-identity-v1`, with
`build_matches=true` and 8/8 anchors. All mods initialized; the redirect registry
published one entry; the first `_Fiopen` of `asset/asset` at 298 ms reported
`redirected:true` and opened `.tmp/asset-proof/asset.patched`. The hand dispatch
patch then installed after eight readiness polls, and the host recorded the
8-byte image write at RVA `0x3d9ac8`.

The game remained at its animated title menu with “Connecting to leaderboards”.
Mouse clicks and Enter did not enter gameplay. The existing Wine input probe could
not attach because its client reported a Steam Proton wineserver version mismatch
(`931/965`). No `hand2.capture` was produced. Therefore this run proves the new
proxy was loaded, archive redirection succeeded, and the draw hook became active;
it does not prove the modified mesh was requested, decoded, drawn, or unused. The
local title-menu screenshots, logs, build, archive and both deployment journals
remain under ignored `.tmp/hand-capture/` and `.tmp/asset-proof/`. The temporary
installation changes were restored; the installed archive and executable hashes
remain unchanged, and all 23 paths in the second deployment journal match their
pre-run state.

## Proton gameplay input diagnosis (2026-11-04)

The previous screenshot shows an **active animated title menu**: PLAY, OPTIONS,
TRAINING, CLIPS, LEADERBOARDS and EXIT are visible while “Connecting to
leaderboards” appears as a status overlay. That message alone does not establish
that online initialization blocks gameplay. The earlier failed navigation is
inconclusive because the helper was launched with host Wine, whose client
protocol did not match the Proton wineserver (`931/965`); it does not prove that
the game ignored correctly delivered input.

HYPER DEMON's compatdata `config_info` points at Proton Experimental. The
reusable input helper detects the app's active Proton wineserver and
`STEAM_COMPAT_TOOL_PATHS` from the matched game process. It checks the
wineserver associated with the target prefix and refuses a mismatch. Runtime
fallbacks remain available for `--inspect` and the isolated fixture, but live
input requires one uniquely matched game process and a runtime tied to that
process/prefix. It does not inspect or interact with another game's prefix. The Windows probe
identifies a window by title plus the executable module enumerated from its
process ID, verifies foreground focus before sending input, and computes menu
coordinates from the client rectangle. It uses
`SetCursorPos`/`mouse_event`/`keybd_event`, not synthetic window messages.

The isolated `proton_input.py --self-test` ran through the same Proton
Experimental build in disposable `.tmp/` compatdata. Its local Win32 fixture
received both the click and Return, changed its title to `HYPRLODR INPUT OK`,
and the probe verified that response. This confirms the Win32 input route and
version-compatible Proton invocation without launching HYPER DEMON. It does not
yet establish that the game's menu accepts the input. The trace and fixture logs
are in `.tmp/proton-input/selftest-run/`.

### Display-environment regression fix (2026-11-04)

The helper previously selected the right Proton prefix but left `DISPLAY` and
`XAUTHORITY` inherited from its caller. That was incorrect for the recorded
working arrangement where Steam/HYPER DEMON ran on Xvfb `:99` and Gunfire
Reborn remained on `:0`. Runtime resolution now carries the matched game's
`DISPLAY`, `XAUTHORITY`, and `HOME` (including `HOME/.Xauthority` when the
process uses Xlib's default cookie path) into the actual Proton child process.
An explicit `--display` is supported; switching away from the game's display
requires an explicit matching `--xauthority` path or `--xauthority none`.
Unrelated shell Xauthority values are discarded.

The helper rejects multiple matching game PIDs, a stale or changed PID, a
missing game display, an unresolved active Proton runtime, and wineserver/runtime
mismatches. It revalidates the selected PID and X11 connection immediately
before launching the Win32 probe. Diagnostic output names the game PID, display,
Xauthority source, Proton version/path, prefix, and input target.

Regression tests cover shell `:0` versus game `:99`, another Proton process on
`:0`, display/authority overrides, missing authority, game-home default
authority, absent/stale/ambiguous processes, and runtime mismatches. One test
launches a fake Proton wrapper and inspects the environment from the child
itself. The real isolated fixture also passed with Proton Experimental on its
own Xvfb display and confirmed mouse plus Return delivery. This verifies helper
environment propagation and Win32 input delivery to the fixture; no new
HYPER DEMON gameplay attempt was made in this change.

### First persistent native hand-buffer capture (2026-11-04)

The next controlled run used the matched game process's `:99` display and
Proton Experimental prefix. `input-play.report` identifies the visible GLFW
window as `hyperdemon.exe`, verifies it was foreground, and records that the
historical Win32 `play` sequence was sent. The capture file was written 359 ms
after that report. The saved frame 480 shows the title menu, but it was taken
before the input action; the post-input screenshot is black. No screenshot or
game-state event records a gameplay transition. Therefore the input action is
confirmed, while entering gameplay and the capture occurring during gameplay
are **not confirmed**.

The persistent raw file is
`.tmp/hand-capture/runtime/hand2.capture` (21,992 bytes, SHA-256
`5063fc8e13b3e4ba5f454f8a9416877be2476217be701c350e7a070eea314d77`). The
existing `tools/re/hand_capture.py` parser validates its 104-byte `HYPRCAP1`
version-1 header and exact length: 238 vertices × 72 bytes (17,136 bytes),
1,188 32-bit indices (4,752 bytes), plus the header. It reports
`GL_TRIANGLES`, draw count 1,188, index offset 0, live buffer IDs 41 and 42,
and attribute offsets `[0,12,24,32,48,60]`. All indices are in range. The
vertex-array SHA-256 is
`de8fcd51455db5e907a6e4682568f8d2e62ddce0b4e0b33e61e0c90707f8a708`; the
index-array SHA-256 is
`1af9f93e699537f502ffc48e81754d5f77b6f8f38ecc0da4a4214cd480832536`.

The corrected offline comparison is
`.tmp/analysis/hand2-live-comparison.json`. The local decrypted tag-1 source
record is at offset `295758784`, size `21972`; its original payload SHA-256 is
`ddfe1797efd29c1200e1c775d998b73ef73b9b0e8710e92d7b632c3a29c93fa8`, and
the edited payload SHA-256 is
`9b0e9cb0e446c56c481547c90994e707fd22f899e0a14e536eb039075328d795`. The
runtime resolver-table capture identifies that offset/size as tag 1 named
`hand2`; a separate tag-2 `hand2` is distinct.

The captured index bytes equal both source arrays exactly. Against the edited
source, the complete vertex array is byte-identical, including positions,
normals, UVs, the uninterpreted 16-byte attribute at offset 32, tangents and
bitangents. Against the original, all non-position attributes are exact after
the analyzer's unique remap keyed by UV0 and offset-32 bytes; topology also
matches. The fitted original-to-capture affine transform has diagonal scale
`1.34999999` (per-axis `1.3499999998`, `1.3499999928`, `1.3500000091`) and
translation approximately `(0.00254956, 0.01428750, -0.00005941)`. Position
RMSE is `4.09e-9`, maximum residual `1.28e-8`. This is an exact byte-level
match for the 1.35× edited mesh in the draw buffer, rather than a scale-
normalized similarity. It strongly supports that the edited geometry reached
the recognized hand draw. Because the capture is from the final OpenGL buffer
rather than decoder output, the remaining narrow alternative is an identical
CPU-side 1.35× position rewrite applied to original source geometry; no such
rewrite is observed in the current logs. A future distinguishability test
should use a non-uniform per-vertex displacement that cannot be represented by
one global object transform, then compare the raw draw buffer with both
payloads.

The crash dump was produced roughly two seconds after the input report and
capture. Its exception is `0xC0000005` at
`hyperdemon.exe+0x7c67`; exception parameters identify a read at address
`0x90`. Ghidra resolves the instruction in `FUN_140007640` (entry RVA
`0x7640`): the code loads an object from a global object's `+0x80` field,
reads its vtable, then calls slot `+0x90`. The dump context has `RAX=0`,
consistent with the null vtable used by that indirect call. The function lacks
useful symbols; the immediate null dispatch is confirmed, but its owning
subsystem and full caller chain are unresolved. There is no evidence tying it
to the asset redirect, passive capture, or Win32 click. The events establish
order, not causation.

The experiment restore journal reports `restored: true`; all 23 journaled
paths were independently checked against saved pre-run SHA-256 or absence,
including the proxy, mods, mode/config files and runtime logs. The installed
executable and original archive also match their preflight hashes. The crash
dump was not a journaled path, so its original pre-run presence is unknown; its
evidence copy is retained under `.tmp/hand-capture/input-env-regression/`, and
the installation copy was left untouched rather than deleting a file whose
prior ownership state is not recorded.

The process selector now treats `/proc/<pid>/comm` as authoritative whenever
real process records provide it: exact `hyperdemon.exe` tasks win over wrapper
argv, and wrapper-only candidates are rejected. This prevents selecting a
Proton Python or `steam.exe` process that merely carries the game path. The
recorded `:99` Xvfb PID is no longer alive. Future managed runs must keep their
owned Xvfb and window manager alive through Steam startup, input and capture;
verify the recorded PID plus `xdpyinfo` and GLX responses before use; and only
tear down processes that the run started. A display number or stale PID file
alone does not establish that the display is still available.

## Size-aware passive capture and next authenticated run

Capture mode in `example.hand-hook` accepts exactly two layouts: original
238/1,188 and enlarged 239/1,191. Both require stride 72, six enabled float
attributes at offsets `[0, 12, 24, 32, 48, 60]`, `GL_TRIANGLES`, and
`GL_UNSIGNED_INT`. It reads only the logical model byte counts (17,136/4,752
or 17,208/4,764), after checking buffer capacities, buffer IDs, aligned index
offset, and index range. The array-buffer binding is restored, and the native
draw is forwarded without changing its arguments. The one-shot deferred writer
uses the selected dimensions in the existing HYPRCAP1 header. Non-capture
modes still recognize only the original 1,188-index hand draw.

When an authenticated session and verified writable-state protection are
available, perform two fresh, separate runs:

1. **Original-size control:** replacement routing disabled; select passive
   capture, enter gameplay until the native hand is drawn, then validate the
   file with `tools/re/hand_capture.py`. Expect 238 vertices and 1,188 indices.
2. **Enlarged replacement:** start fresh with the validated append-only archive
   and exact tag-1 route active before the first cold request. Correlate archive
   readiness, replacement extent, successful native loader return, and parsed
   239/1,191 counts with a 239/1,191 GPU capture. Confirm a 1,191-index draw,
   then inspect gameplay for visible added geometry.

For the enlarged run, retain evidence in this order: verify the executable and
original archive hashes; validate the replacement archive and manifest; record
archive-gate `READY` before the first cold tag-1 request; record selection of the
replacement descriptor and the native loader return; confirm the decoded
239/1,191 counts; analyze the 17,208-byte VBO and 4,764-byte IBO capture; then
confirm the draw count and inspect an actual gameplay frame. The passive capture
recognizes a strict GL signature (counts, attributes, capacities, IDs and range)
but has no native resource pointer at draw time, so a same-layout unrelated draw
could still match. Correlate process, loader, parser and capture timestamps and
buffer IDs; do not treat dimensions alone as resource identity. Parsed geometry,
GPU upload, draw submission and visible geometry are four separate claims.

The compiled Windows/Wine regression feeds files emitted by the production
capture writer directly to the Python CLI and compares them with bytes read from
the fixture's fake GL storage. Both approved signatures pass this native-writer
to production-parser boundary, including the enlarged additional vertex and
triangle. This is synthetic evidence only; it is not evidence that the enlarged
replacement reached the real game's GPU.

For the controlled authenticated experiment, use the concise
[`HAND2_RUNTIME_RUNBOOK.md`](HAND2_RUNTIME_RUNBOOK.md). It separates vanilla,
original-geometry, and enlarged-replacement runs and requires user verification
of prefix and Steam Cloud boundaries before any deployment.

Keep parser acceptance, GPU upload, draw submission, and visible geometry as
separate evidence. Before either run, snapshot applicable game configuration
and save files, verify executable/archive/proxy identity and the reversible
deployment journal, confirm experimental writable paths do not alias the normal
Proton prefix, and resolve Steam Cloud uncertainty. Do not run if test state
could synchronize to the user's account.
