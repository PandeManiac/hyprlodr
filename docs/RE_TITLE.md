# Title pipeline: what is confirmed so far

Everything here is **Observed** unless a line says otherwise. Decompiled or disassembled
game code is never committed; only addresses and structure are recorded.

Binary: `hyperdemon.exe`, sha256 `7612dd72…3611` (4,175,360 bytes), image base `0x140000000`.

## Confirmed: the lobby entities are registered by name at start-up

Both references to the only lobby-entity strings in the binary are the *same* sequence,
repeated once per entity:

```asm
1401208a2: lea  0x24ae5f(%rip),%rdx     # 0x14036b708  "entity_lobby_dagger_hyper2"
1401208a9: lea  0xab0(%rbp),%rcx        ; stack std::string
1401208b0: call 0x140002d20             ; construct string from literal
1401208bd: mov  0x8d40(%rbx),%rcx       ; engine/lobby context
1401208c4: call 0x140211bc0             ; resolve entity by name
1401208cc: lea  0x6948(%rbx),%rcx       ; destination slot (array, stride 0x70)
1401208d3: call 0x140211720             ; store resolved entity
1401208e0: call 0x140002cd0             ; destruct temporary string
```

and at `0x1401208e5` the same block with `"entity_lobby_dagger_hyper"` (`0x14036b6e8`) into
slot `0x68d8(%rbx)`.

**Observed:** names are resolved through a helper at `0x140211bc0` and stored into consecutive
fields with a 0x70 stride (`0x68d8`, `0x6948`, `0x69b8`), i.e. an array of lobby-entity
entries built once at start-up.

**Inferred:** lobby content — including the title presentation — is addressed by *entity
name*, not by a string of the words shown. This is consistent with `HYPER`/`DEMON` existing
nowhere in the binary while the menu labels (`PLAY`, `OPTIONS`, …) do.

## Confirmed: `shaders` is a *registered name with a callback*, not a directory lookup

The `shaders` anchor (`0x1403652f8`) is referenced twice, in functions at `0x1400019b0` and
`0x140071990`:

```asm
1400019b4: mov  $0x8,%ecx
1400019b9: call 0x1402feb84             ; allocate a registration object
1400019be: lea  0x3692eb(%rip),%rcx     # 0x14036acb0
1400019c5: mov  %rax,%r8
1400019c8: lea  0x363929(%rip),%rdx     # 0x1403652f8  "shaders"
1400019d2: lea  0x3d8b77(%rip),%rcx     # 0x1403da550
1400019d9: call 0x140004ac0             ; register (directory, name, thunk)
```

**Observed:** the literal `shaders` is passed with a second string and a pointer to
`0x1403da550` into a registration routine at `0x140004ac0`. The only shader *file* name in the
binary is `skull_giga.vert` (`0x1403689d0`), which has **no** RIP-relative reference — it is
reached through data, not code.

**Inferred:** shader sources come from the `shaders` directory inside the asset container,
which is still undecoded — hence the shader text cannot be read statically.

## Confirmed: renderer and the limits of interception

- GLFW + OpenGL: `opengl32.dll` → Wine → Mesa GLX, context `4.6 (Compatibility Profile)`
  (in-process query, `docs/evidence/2026-11-01-gl-capture.json`).
- `wglSwapBuffers` *is* resolved through `GetProcAddress` and can be shimmed; the swap callback
  receives a live context and a 1920×1080 `glReadPixels` readback works.
- The engine requests **no** `gl*` name through `GetProcAddress`/`wglGetProcAddress`, so its own
  GL dispatch stays outside our interception (`docs/GL_CAPTURE.md`).

## Not established (needs a running game)

- Whether the title letters are an atlas of glyph quads, one wordmark texture, or the
  `entity_lobby_dagger_hyper*` meshes.
- Which shader produces the iridescent material and what drives its animation.
- The title's draw calls, their order and their render targets.

## Tooling note

`analyzeHeadless` aborted with `java.io.FileNotFoundException: Directory not found: /tmp/gp2`
(the project directory must exist before launch). The findings above come from `objdump` over
byte slices at mapped file offsets, which is reproducible and needs no project:

```
objdump -D -b binary -m i386:x86-64 --adjust-vma=<vma> slice.bin   # works
llvm-objdump -d --start-address=<vma> ...                         # empty for this PE
```

## Correction and decompilation (Ghidra, 2026-11-01)

Decompiling the two helpers resolves the ambiguity in the section above. (Decompiled
code is game content: it is read locally and only its structure is recorded here.)

**`FUN_140004ac0(name, extra, callback)` is a name→callback registry.** Observed
behaviour: it lazily allocates a 0x40-byte global at `DAT_1403d91e0`, builds a
`std::string` from the second argument, calls `FUN_14000c4d0(registry, &string)` to get
a value slot, writes the third argument (the callback) into it, then destroys the
temporary string (SSO-aware, `free` only when `0xf < size`).

So the earlier reading of `("shaders", 0x14036acb0, 0x1403da550)` as "shader directory"
is **superseded**: it registers the name `shaders` against the callback at
`0x1403da550`, in a global string-keyed table. It is a named facility registry
(script/command/module style), not a path fetch.

**`FUN_140211bc0(context, name)` is a typed registration wrapper.** Its whole body is
`FUN_140211e70(context, 4, name)` — the `4` is a kind/type tag. Its caller stores the
result into a 0x70-stride array slot (`0x68d8`, `0x6948`, `0x69b8`), and Ghidra typed the
wrapper `void` although the caller consumes `RAX`, so the helper returns the resolved
object; that part is recorded as caller-derived, not from the decompiled signature.

**Implication for the replacement:** lobby entries are produced by a typed resolver
(kind 4) into a fixed array at start-up, and facilities are looked up through a named
registry. Replacing the title therefore means changing what the `entity_lobby_dagger_*`
entries *contain* — a texture, a mesh, or a glyph set — not renaming anything.

## Reproducing the decompilation

```bash
mkdir -p /tmp/gp
/opt/ghidra/support/analyzeHeadless /tmp/gp hd -import <game>/hyperdemon.exe \
  -analysisTimeoutPerFile 300 -scriptPath tools/re/ghidra \
  -postScript DumpAt.java 0x140211bc0 0x140004ac0
```

The project directory must exist before launch (otherwise Ghidra aborts with
`Directory not found`), and analysis took ~2 minutes for this 4 MB PE.

## The asset resolver, decompiled (Ghidra, 2026-11-01)

`FUN_140211e70(ctx, kind, name, flags)` — what the lobby registration sites actually call:

- fetches a per-kind table with `FUN_140213b70(ctx + 0x128, &kind, name, flags, -2)`;
- binary-searches the entry for `name`: the key is a `std::string` read SSO-aware from
  `+0x20` (buffer), `+0x30` (size), `+0x38` (capacity), compared with `memcmp` over the
  shared length and then by length;
- **on a miss** it calls `FUN_140213cb0` — the already-known "Can't find asset " builder —
  and logs through `FUN_140075b70`/`FUN_140075d40` with the format at `0x140365b70`;
- **on a hit** it materialises a record with `FUN_140212040(&local, entryIndex * 0x58 + *table)`
  (so entries are 0x58-byte records, indexed by the `int` stored at entry `+0x40`), loads it
  with `FUN_140212920(ctx, record, name, flags)`, then fills an output structure
  (fields at `+0x00`, `+0x04`, `+0x08`, `+0x10`, `+0x18`, `+0x20` string, `+0x40`, `+0x48`,
  `+0x50`) via `FUN_140213950`.

`FUN_14000c4d0(registry, key)` — the facility registry lookup — is the same shape: sorted
`std::string` keys with `memcmp`, returning the value slot; on a miss it inserts a new entry
(`FUN_14000e630` for the value vector, `FUN_14000e4a0` for the key vector) and returns the
fresh slot, which is why `FUN_140004ac0` can just write its callback into the result.

**Observed consequence:** every entity/asset resolution is a `(kind, name)` → `0x58`-byte
record lookup in a sorted per-kind table under `ctx + 0x128`, and a miss is reported loudly
as "Can't find asset <name>". **Inferred:** replacing the title means making the lookup for
`entity_lobby_dagger_hyper*` (kind 4) yield replacement content — either by redirecting the
asset the record points at (the `_Fiopen` redirect already proven in the loader) or by
swapping the record in memory; nothing about the lookup itself needs to change.

## The asset record, decompiled (Ghidra, 2026-11-01)

`FUN_140212040(dst, src)` is a pure field-by-field copy of a **0x58-byte record**, so the
layout is now known:

| Offset | Width | Notes |
|---|---|---|
| `0x00` | u32 | copied verbatim |
| `0x04` | u32 | copied verbatim |
| `0x08` | u8 | copied verbatim |
| `0x10` | u64 | copied verbatim |
| `0x18` | u64 | copied verbatim |
| `0x20` | `std::string` | buffer/size/capacity at `+0x20`/`+0x30`/`+0x38`, copied with `FUN_140002fd0` |
| `0x40` | u32 | **type/kind tag** |
| `0x48` | u64 | copied verbatim |
| `0x50` | u64 | copied verbatim |

`FUN_140213950(table, name)` is the same sorted-map slot lookup as `FUN_14000c4d0`
(SSO-aware `memcmp`, insert-on-miss), returning the writable slot for a key — which is how
the loaded record is written back after being materialised.

`FUN_140212920(ctx, record, name, flags)` is the loader, and it dispatches on `record[0x10]`
(i.e. the type tag at `0x40`). Observed cases:

- **type 1** — lazily allocates a 0x38-byte object: initialises three pointers to zero, calls
  `FUN_140206820()` for one field, then allocates a 0x50-byte sub-object initialised with
  `+0x08 = 0x20` and zeroed fields; the pointer is cached at `record+0x90`. If it is already
  present, the function tests a bitmask at `*(u32*)(cached->next + 0x4c) & flags` and returns
  early when the bit is set — i.e. load-once-per-flag semantics.
- **type 2** — takes the pointer at `record+0x90` (null-checked) and allocates a 0x40-byte
  object, filling further fields.

**Confirmed:** the record carries a type tag plus a `std::string` (at `+0x20`) and two trailing
pointers (`+0x18`, `+0x48`, `+0x50`), and the loader builds a different object graph per type.
**Inferred:** the string is the asset's own name/path inside the container, and the type tag
distinguishes texture-like from mesh-like content — which is exactly the fork the title
replacement needs. Proven by the container still being undecoded: the string cannot be read
statically beyond its presence.

## Confirmed: the game's fonts are pack records, and the title names are material graphs

With the pack readable (see `RE_PACK.md`) the title's neighbourhood is now identifiable, and two
new type tags appeared that the static dispatch reading did not show:

- **`tag 4096` is a TrueType font** (eleven records, each a `0x18` header plus a full 22-table
  sfnt). So the glyph shapes the menu and title are drawn with are *data*, not code.
- **`tag 8`** is a second named-graph type. The 24 names `6eye, bronze, gold, hand1bones,
  hand1fract, hand1nerves, hand1spec, hand1veins, hand1veinsgem, hand2, hand2bones, hand2fract,
  hand2nerves, hand2spec, hand2veins, **hyper**, **hyper2**, silver, soratheye, sorathmask,
  sorathtexture, sorathvector, unranked, unranked2` map one-to-one onto 24 contiguous records,
  and the `hyper` / `hyper2` payloads decrypt to graphs whose sub-object names include
  `projection`, `retrieval` and `entity`.

**Consequence for the replacement.** `HYPER`/`DEMON` are absent from the executable because the
lettering is *text drawn with these fonts* under an entity whose skin is one of the `hyper`
graphs — not a wordmark bitmap. A replacement that renders `HYPR`/`LODR` should therefore go
through the same font/glyph path and the same material, which is exactly the reuse the brief
requires; nothing needs a text overlay.

**Not yet established:** which of the eleven fonts the title uses, and whether the title's *string*
also comes from the pack (a `tag 4` length-prefixed string) or is assembled by the lobby entity.
The `hyper` material graph is the place to look next, now that it decrypts.

## Confirmed: `tag 8` is GLSL material source, and the title's words are stored nowhere

Cracking the record table and dumping every slice's readable strings gives a corpus of **22
shader programs** (`tag 8`). They are the engine's material system, one program per surface,
specialised by preprocessor defines:

```
#if defined( SKINNED )            #if defined( LOBBY_DAGGER_HYPER2 )
#ifdef THREAT_FLARE               #ifdef CLIP_PLANES / ERODE / SHADOW_FLUID
#if defined( GHOST ) || defined( REVEAL ) || defined( SNAKE ) || defined(HAND_VS)
#ifdef OWLWING / BIRDWING / GEM
```

with a shared uniform block — `world_matrix`, `view_proj_matrix`, `spherical_proj`,
`view_position`, `accum_time`, `global_time`, `iri`, `crim_colour`, `erode`, `hit_data`,
`wave_rings` — and a texture set `tex_diffuse`, `tex_mask`, `tex_edgespec`, `tex_spec`,
`tex_reflection1..3`, `tex_danger`, `tex_dashable`, `tex_pace`, `tex_lights`. That is the
iridescent material the title wears, in source form, and the spherical projection that makes the
lettering bulge is an explicit vertex-shader step (`project_spherical`).

**The decisive negative.** A scan of every record's decrypted payload (ASCII, UTF-16BE and
UTF-16LE) finds the literal `HYPER` in exactly one place: the shader define
`LOBBY_DAGGER_HYPER2`. `DEMON` appears nowhere. Combined with the earlier fact that neither word
exists in the executable, the title's lettering is **not a stored string and not a bitmap** — it
is geometry drawn with the game's fonts (Georgia for text, `tag 4096`) and shaded by an
`entity_vs`/`entity` material. A replacement therefore has to change *glyph content or glyph
geometry*, and the natural instrument is a same-tag, same-length payload served under the same
name.

## Confirmed: the pack holds no image data — the title is geometry, shaded

The record table recovered from the live process is **complete**: its 863 spans tile the whole
306,794,369-byte pack with no overlap, so nothing is hiding outside the records.

Two surveys of the decrypted payloads settle what the pack does *not* contain:

- **No image container** anywhere: no `DDS `, `PNG`, `KTX `, `JPEG`, `RIFF` or `TGA` signature in
  any record's first 512 bytes (and the eleven `tag 4096` slices are genuine TrueType, so the
  scan reads payloads correctly).
- **No raw image geometry** either: searching every record for a width/height pair near the start
  whose extent accounts for the remaining bytes at 1–4 bytes per pixel (`tools/re/find_textures.py`)
  yields **zero** candidates across all 863 records and across the 39 slices whose seed is pinned
  by name.

Together with the earlier negative — `HYPER`/`DEMON` exist nowhere in the pack or the executable —
this rules out the two easy readers of the capture. The title's letterforms are **not a mask
texture and not a stored string**. They are geometry drawn by the `entity_vs`/`entity` material:
the giant letters are shaded by the same iridescent program as everything else (hence the
`spherical_proj`, `iri`, `crim_colour` uniforms and the ten-texture set), and the vertical
repetition visible at the capture's edges is the same letterform drawn at a coarse tile pitch,
not a wrapped bitmap.

**Method caveat, measured.** For a slice whose name is unknown the seed is inferred from content
(which key class yields the most readable text). That inference is validated: on the 39 slices
whose name is known, the inferred class equals `zlib.crc32(name) & 0x3ff` for **10/10** that are
reachable this way, including the binary `tag 1` and `tag 2` payloads — so the surveys above are
not noise. What *is* noise is scoring records by how many uppercase letters they contain: that
test matched 837 of 863 records and was discarded rather than used.

**Consequence for the replacement.** Changing `HYPER`/`DEMON` to `HYPR`/`LODR` means changing
glyph geometry (or the glyph source the geometry is built from), then serving a same-length,
re-encrypted payload under the same name — the path `tools/re/pack_patch.py` already proves
byte-exact. A text overlay or a replacement image would be a different renderer, which the brief
excludes.

## Confirmed: the material set, and a program called `title_vs`

Every `tag 8` slice **self-identifies**: its payload starts with the asset's own name, and
`zlib.crc32(that name) & 0x3ff` reproduces the key class the slice decrypts under. That is a
closed loop — a name is only accepted if its CRC regenerates the seed — and it yields the whole
material set without any array pairing:

| material | bytes | record (seed) |
|---|---|---|
| `entity` | 101676 | 306470982 (seed `0x068`) |
| `post` | 57003 | 306654293 (seed `0x08d`) |
| `tile` | 41389 | 306741738 (seed `0x104`) |
| `particle` | 36465 | 306611973 (seed `0x3e1`) |
| `dev` | 12696 | 306456816 (seed `0x105`) |
| `snake` | 10730 | 306724525 (seed `0x39e`) |
| `warp` | 8165 | 306783127 (seed `0x066`) |
| `projection` | 7313 | 306711296 (seed `0x026`) |
| `gem` | 7039 | 306572658 (seed `0x2b0`) |
| `lazer` | 6794 | 306595706 (seed `0x343`) |
| `hologram` | 6782 | 306588924 (seed `0x351`) |
| `portal` | 5855 | 306648438 (seed `0x3f0`) |
| `gui` | 5847 | 306581880 (seed `0x0f8`) |
| `owl` | 5047 | 306606926 (seed `0x34d`) |
| `squid` | 4558 | 306735255 (seed `0x1ef`) |
| `light` | 4426 | 306602500 (seed `0x0f7`) |
| `retrieval` | 4420 | 306718609 (seed `0x0aa`) |
| `gem_old` | 2183 | 306579697 (seed `0x203`) |
| `thorn` | 1925 | 306739813 (seed `0x1c8`) |
| `egg` | 1470 | 306469512 (seed `0x142`) |
| `skinning` | 1443 | 306723082 (seed `0x0f6`) |
| `hand` | 1197 | 306587727 (seed `0x28f`) |
| `boss` | 513 | 306456303 (seed `0x23a`) |

These are shader *programs* (a slice holds several named vertex/fragment pairs), not single
shaders. The two that matter for the title:

- **`gui`** contains a program named **`title_vs`**. Its vertex shader ignores the world matrix
  and emits `gl_Position = vec4( in_position.xy, 0.0, 1.0 )` — a **full-screen quad** — and the
  paired fragment shader computes `vec3 result0 = mask * bg;` and writes it with alpha 1. The
  title is therefore a screen-space composite of a *mask* over a *background*, which is exactly
  the look in the capture: letterform silhouettes multiplied against the iridescent field.
- **`post`** (57,003 bytes, 1,465 lines) is the post-process chain and contains a pass named
  **`post_title_fs`** — a straight `texture(tex_diffuse, vuv)` copy — plus `post_combine`,
  `post_blur_horz`, `post_glow_blur_vert`, lens-flare, vignette and sobel passes.

**Correction (supersedes "the title is geometry").** Reading the whole `title_vs` program shows
the letterforms are a **texture alpha channel**, not geometry. The fragment shader samples three
textures and does exactly this:

```glsl
uv1 = uv0;                                       // full-screen uv
uv1.x -= (data.x - 854.0) * 0.5 / data.x;        // centre an 854-wide window
uv1.x  = uv1.x * (data.x / data.y) / (854.0/480.0);   // aspect-correct that window
vec3 mask = vec3( 1.0 - texture( tex_diffuse,  uv1 ).a );  // letters = *inverse alpha*
vec3 bg   =        texture( tex_diffuse3, uv0 ).rgb;       // iridescent background
if ( uv1.x < 0.0 || uv1.x > 1.0 ) mask = vec3( 0.0 );      // clip to the window
out_colour0 = vec4( mask * bg, 1.0 );
```

Three facts follow, and they are the whole title pipeline:

1. The title's lettering is the **alpha channel of `tex_diffuse`**, used inverted (`1.0 - a`), so
   the glyphs are the *transparent* parts of that texture and the surround is opaque. That is why
   the capture shows black silhouettes cutting into the iridescent field: the field is `bg` and
   the letters are where `bg` is **not** multiplied in.
2. The mask is composited over a **854×480 window**, centred horizontally by `data.x`, then
   aspect-corrected to the drawable. The apparent repetition/stretch at the capture's edges is
   this window being stretched to 1920×1080, not a wrapped bitmap.
3. `bg` is a separate full-screen texture (`tex_diffuse3`) sampled with plain screen UVs, so the
   iridescent animation lives in the background texture and is untouched by the mask.

The earlier "no image in the pack, therefore geometry" step is therefore wrong in its conclusion
though right in its evidence: there is no *stored* mask because the mask is a **render target**.
The same `gui` slice also contains `gui_text_fs`, which multiplies a font mask into a colour and
writes it with **alpha 0.5** — i.e. GUI text is rasterised into offscreen targets, and the title
pass consumes one of them. The glyph shapes in that target can only come from the pack's eleven
TrueType fonts, which are the game's only glyph source.

**Plan this makes concrete (design, not yet verified).** `HYPR`/`LODR` does not need a new
renderer: it needs the text that is rasterised into the title's alpha target to be different
characters. Reusing the game's own path means: keep `title_vs` and the `post_title` copy
untouched, keep `tex_diffuse3` (the background) untouched, and change only the glyph rasterisation
that fills the mask — same font, same alpha convention (`1.0 - a`), same 854×480 window. The
remaining unknown is the CPU-side string and font selection for that pass, which is exactly what
the next step targets.

## Where this leaves the title (state of knowledge)

- The title is content behind `entity_lobby_dagger_hyper*` (kind 4), resolved as
  `(kind, name)` → 0x58-byte record → loader by type tag.
- It is **not** a text string: `HYPER`/`DEMON` do not exist in the binary, while the menu
  labels do.
- Nothing yet says whether the visible letters are a texture, an atlas or meshes; that needs
  the running game, which needs a Steam session.

## Records are typed slices of the asset stream (Ghidra, 2026-11-01)

Reading `FUN_140212920` to its end settles what a record *is*. The trailing half of the
function does the actual work:

- `malloc((int)record[0x04])`, then `seekg(*record)` on the stream passed as the first
  argument — so **`record[0x00]` is a byte offset and `record[0x04]` is a byte count**;
- `FUN_140215280(0, name, len)` computes a check value over the record's *name*, then
  `FUN_140213370(stream, buffer, size, check)` reads the slice (validating it);
- a second `FUN_140215280` over the payload is added to the running byte counter at
  `stream + 0x168` (pack-read accounting);
- the payload is handed to a per-tag parser starting at `buffer + 0x18`, i.e. **payloads carry
  a 0x18-byte header** (`record[0x20] - 0x18` is the data length for tag `0x20`);
- the stream object (`stream + 0x120`) is passed along for tag `0x100`.

Tag → parser, as read from the dispatch:

| Tag | Parser | Observed character |
|---|---|---|
| `1` | `FUN_140205ec0` | 404 lines, richest: builds a named sub-object graph (`FUN_1402067a0`, `FUN_140206870`, `FUN_140206900`, `FUN_140207280`, `FUN_14021a710`, `FUN_14023d6c0`) |
| `2` | `FUN_1402330d0` | 87 lines, plain struct reader over u16/u32 fields with a 16- vs 32-bit variant gate |
| `4` | `FUN_140210080` | 146 lines: length-prefixed string then integers (char-buffer construction on the stack) |
| `0x20` | `FUN_140208790` | 552 lines, largest parser; parses a text-like format (`FUN_1400ab750`/`FUN_1400ab7c0`) |
| `0x100` | `FUN_1401cd390` | **audio, confirmed**: `alGenBuffers`/`alBufferData` (OpenAL), buffer id in `obj+0x40`, sample rate `obj+0x7c`, mono/stereo via `0x1101`/`0x1103`, downsampling when the rate exceeds `DAT_140375890` |

**Confirmed:** `entity_lobby_dagger_hyper*` names resolve to records of
`(offset, size, tag)` into the asset stream, and the tag chooses the parser that materialises
them. **Hypothesis (not proven):** the title's records use tag `1` (named sub-object graph) or
`2` (compact struct). **Ruled out:** tag `0x100` is audio, and tags `4`/`0x20` are
string/format parsers, so none of those can be the lettering itself.

The pack format, cipher and the remaining unknowns are written up separately in
[`RE_PACK.md`](RE_PACK.md).

Why this matters: the lettering is addressed *by name inside the pack*, so a replacement can
be a same-tag payload served under the same name — which is exactly what the `_Fiopen`
redirect layer is for. What remains unknown is the record's tag and its payload layout, and the
pack (`asset/asset`, 306,794,369 B) is still undecoded, so neither can be read offline.

## Confirmed in-game: the title's mask texture, and a replacement drawn by the original pass (2026-11-01)

**The title's lettering is one texture, and it has been read out of the running game.** Binding
every texture name at `wglSwapBuffers` and reading level 0 with GL 1.1 entry points found a
texture of exactly **853x480** whose **RGB is zero everywhere** and whose **alpha channel is the
`HYPER` / `DEMON` letterforms** — 256 alpha levels, 42.8% of pixels inked, two bands separated by
a horizontal gap at rows 233..247. That is the texture `title_vs` samples: the fragment shader
takes `1.0 - alpha` as the mask and multiplies it by the full-screen background inside its
854x480 window. The window constant and the texture size agree, which closes the loop between the
shader text and the live renderer.

**Why this took a new instrument.** The executable imports no GL library at all: its import table
lists kernel32, user32, MSVCP140, OpenAL32 and friends, but no `opengl32`, no `vulkan-1`, no
`d3d11`. Through the loader paths that can be patched it asks only for `wglChoosePixelFormat`,
`wglSetPixelFormat`, `wglDescribePixelFormat` and `wglSwapBuffers`. So the engine resolves its GL
entry points in a way no IAT hook can see, and per-draw tracing — shader sources, draw calls,
texture uploads — is unobtainable from user mode with this approach. That is a measured negative,
not an assumption: patching every module's `GetProcAddress` (whatever DLL it comes from), every
`wglGetProcAddress`, and every direct `opengl32` import still produced **zero** shader, program or
draw events across full sessions. The swap hook it does use is enough to observe renderer state,
and that is what produced the mask.

**The replacement reuses the original pass.** Since the mask is an ordinary 853x480 RGBA texture
and the engine never re-uploads it, writing a different glyph mask into that same texture name
makes the game's own title pass draw the new words: same `title_vs`, same `854x480` window maths,
same animated background texture, same `post_title` copy and the same post chain. Nothing is
composited on top of the finished frame and no second renderer exists.

- `tools/make_title_mask.py` renders `HYPR` / `LODR` into that exact texture layout using one of
  the game's own fonts (`--font`), writing alpha = `255 - coverage` (letters are the transparent
  part, matching the measured mask) and flipping the rows, because the upload path treats row 0 as
  the bottom row. Without the flip the words render inverted, which was observed before fixing it.
- The loader (`native/win32/gl_hook.c`) reads `hyprlodr-title-mask.raw` from the game directory
  and uploads it into the title texture from frame 20 onwards, restoring the previous binding.

**Verified in HYPER DEMON** (world-space captures at 1920x1080, frames 120 and 300 of the same
session): the title area shows **`HYPR` above `LODR`**, upright, filled by the animated iridescent
background, inside the original oversized letterforms, with the menu row `PLAY OPTIONS TRAINING
CLIPS LEADERBOARDS EXIT` untouched and still laid out between the two words. The texture read back
after the game had drawn with it contains our mask (ink 0.312 against the original's 0.428).

**Still hypothesis, not confirmed:** which of the pack's fonts the *original* mask was drawn with
(the glyph shapes resemble a bold Didone serif; the packed font at record 303,451,631 reproduces
the shapes closely but this is not proven), whether the shipped mask is stored packed or built at
load time, and how the game would animate or re-render the mask if the title were re-entered.

## Confirmed: the mask is recognised by signature, and seven textures share its size (2026-11-01)

Targeting the mask by texture name was provisional, because texture names are allocation counters.
Replacing that with recognition by what the texture *is* produced a sharper result than expected:
the live renderer holds **seven** textures of exactly 853x480, and only one of them is the title
mask.

| texture | alpha | RGB | is the title mask |
|---|---|---|---|
| 81 | mixed (ink 0.428 / 0.312 after replacement) | 0 | **yes** |
| 112 | uniform clear | 0 | no |
| 113 | uniform opaque | 0 | no |
| 114 | uniform opaque | up to 255 | no |
| 115, 116, 117 | uniform clear | 0, 3, 5 | no |

A size-only match would therefore have picked 112 — an identically sized, identically zero-RGB
texture — and overwritten something the renderer owns. The predicate that discriminates is the
conjunction of size (853x480), zero colour, and genuinely mixed alpha; the loader now scans for it
and logs `title_mask_found` with the name it settled on (81 in this session, found at start-up).

**Multi-frame evidence from one session** (1920x1080, world-space captures):

- frame 120 — the Sorath splash, before the title exists (`9 players currently online.`, the eye
  mark and the `SORATH` wordmark). The upload has already happened and is harmless.
- frame 300 — `HYPR` over `LODR`, upright, in the original oversized serif, filled by the animated
  iridescent background, with `PLAY OPTIONS TRAINING CLIPS LEADERBOARDS EXIT` between the words.
- frame 480 — the same, and the lettering now carries a small rotation relative to frame 300.

That last point is the animation evidence: the game applies its own animated transform to the title
after the mask is sampled, so a replacement mask inherits the rotation, the background flow and the
post chain without reimplementing any of them.

## Confirmed: the typeface candidates, measured against the shipped mask (2026-11-01)

The pack's eleven fonts are now properly extracted and named, after fixing a swapped pair of
fields in my `name`-table reader (length and offset were read from the same two bytes, which is why
earlier attempts produced one enormous garbage family name and no fonts at all). Every font sits
behind a 0x18-byte payload header, and the family names are exactly the keys already recovered:
**Georgia** (regular, bold, italic, bold-italic), **Goethe Bold**, **Mongolian Baiti**,
**Tinos** (regular, bold), **Consolas**, **Font Awesome 5 Free Solid** and **Hack**.

Each font was then rendered as `HYPER` and `DEMON`, normalised to each band's ink box, and scored by
IoU against the captured original mask:

| font | best IoU | word / size |
|---|---|---|
| **Georgia Negreta (Bold)** | **0.677** | HYPER @ 96px |
| Tinos Bold | 0.638 | HYPER @ 88px |
| Georgia | 0.561 | HYPER @ 72px |
| Tinos Regular | 0.557 | HYPER @ 48px |
| Georgia Negreta cursiva | 0.554 | HYPER @ 60px |
| Mongolian Baiti | 0.552 | HYPER @ 84px |
| Goethe Bold | 0.448 | DEMON @ 56px |
| Consolas | 0.350 | — |
| Hack Regular | 0.332 | — |

**Hypothesis, stated as such:** Georgia Bold is the leading candidate but 0.677 is not identification.
A wrong font of similar class lands far lower (Consolas 0.35, Hack 0.33), and the second place is
another serif of the same genre, so the result narrows the field to "bold transitional/didone serif
from the pack, most likely Georgia Bold" rather than settling it. Proving it would need the glyph
outlines matched, not silhouettes at one normalised scale.

**Confirmed, by measurement:** the shipped mask's ink fraction is 0.428, and the replacement now
fits its glyph scale to that number (`--target-ink`, which converged at scale 1.029). Offline the
generated mask measures 0.424, and the texture read back from the running game measures 0.424
against the original's 0.428, so the replacement no longer reads lighter than the original title.

## The mask's exact geometry, measured (2026-11-01)

Measured from the captured mask (ink = alpha < 128), rows counted from the top of the texture:

| | rows | cap height | columns | ink inside the band |
|---|---|---|---|---|
| upper word | 12..220 | 209 | 12..842 (831) | 0.425 |
| lower word | 249..469 | 221 | 12..842 (831) | 0.459 |

Rows 221..248 are blank, so the gap between the words is **28 rows**, and the margins are 12 rows
above, 10 below, 12 left and 10 right. This corrects an earlier note that put the gap at rows
233..247: that number came from a coarser scan and is superseded.

The generator places the replacement's ink box on exactly this grid (`RECTS`), so the new lettering
occupies the space the original occupied instead of being centred approximately.

At the exact rects the replacement inks 0.378 of the canvas, because four letters cover less area
than five. An earlier revision closed that gap with `--target-ink 0.428`, growing the box about its
centre by 6.5% - and measuring the result shows it breaks the geometry it was supposed to serve:

| mask | bands (cap) | columns | gap | ink |
|---|---|---|---|---|
| original | 12..220 (209), 249..469 (221) | 12..842 | 28 | 0.428 |
| exact grid (**ships**) | 12..220 (209), 249..469 (221) | 12..841 | 28 | 0.378 |
| density fit (rejected) | 5..227 (223), 242..476 (235) | 0..852 | 14 | 0.425 |

The density fit makes the caps 223/235 px against the original's 209/221, bleeds to column 0 and row
476, and halves the gap between the words. Ink density is a *consequence* of the original having five
letters per word, not a property to reproduce: imitating it by scaling the glyphs trades an exact
match on geometry for an approximate match on coverage. The exact-grid mask is therefore what ships,
verified in game (frames 120/300/480 and 491). `--target-ink` remains available for comparison and its
help text now says plainly that it leaves the measured rects.

## Typeface: what the measurement cannot settle (2026-11-01)

Two successive tests, both honest about their limits:

**Test 1, silhouette IoU after normalising each word to the band's ink box.** Georgia Bold 0.677,
Tinos Bold 0.638, Georgia 0.561, Mongolian Baiti 0.552, Goethe Bold 0.448, Consolas 0.350,
Hack 0.332.

**Test 2, each font at the original's own cap height (209/221 px) with letter tracking solved over
-50..+24 px, no glyph distortion at all.** Here the field collapses: Georgia 0.680, Mongolian Baiti
0.672, Tinos Regular 0.669, Goethe Bold 0.663. Four different faces land within 0.017 of each other.

So the honest conclusion is that **silhouette comparison cannot identify the title's typeface**. The
only defensible claim is the one the data supports: it is a bold serif from the pack, and Georgia
Bold is a good stand-in, not a proven match. Settling it would need glyph-outline matching - stroke
skeleton, terminal shapes, bowl curvature - rather than filled silhouettes at a single scale.

**Retraction:** in test 2 the DEMON column reported 0.000 for most fonts. That was a bug in my
harness, not a measurement: the font was sized from `HYPER`'s bounding box, so `DEMON`'s cap height
missed a 4 px tolerance and every candidate was discarded. The DEMON column from that run is void;
the HYPER column, which is what the ranking above uses, sized and compared the same word throughout.

**A note on stroke contrast.** The original has stem/cap 0.196 and bar/cap 0.091 (upper word),
0.186/0.127 (lower): a high-contrast serif. At cap height Georgia Bold measures 0.250 stem (heavier
than the original), Georgia 0.160, Tinos Bold 0.222, Goethe Bold 0.188. That spread shows the
replacement is in the right class without identifying the face either.

## On-demand frame capture (2026-11-01)

Fixed frame numbers cannot follow menu navigation, because how many frames a screen lasts depends on
load time. Writing a serial number into `<game>/hyprlodr-dump-request` now makes the next presented
frame dump itself; the marker is only read, never deleted, because the project's minimal Windows
headers declare neither `DeleteFileA` nor `FILE_SHARE_DELETE`.

**Measured negative, revised:** input injection does not reach the game under this harness, and the
first explanation for that was wrong. I had blamed the absence of a window manager, so I re-ran the
test with `openbox` on `:99`. `wmctrl -l` then listed the client as `0x04600003  hyperdemon` (class
`steam_app_1743850`) and `xdotool getwindowfocus getwindowname` answered `hyperdemon`, i.e. the game
**does** hold X input focus. Input still had no effect: a real XTest click at the centre of `OPTIONS`
(371,532), 24 incremental `mousemove_relative` steps to serve a client that uses a grabbed relative
cursor, `Return`, `Tab`, `Right` and `Escape` all left the title screen byte-for-byte in the menu area
across five capture points (frames 483, 553, 621, 505, 591, 670). The focus hypothesis is therefore
falsified and retracted: it is the game that discards injected keyboard and pointer events, which is
what a client reading XI2 raw input from a physical device would do, since XTEST events arrive on the
virtual device. The menu round-trip stays **unverified**.

Note what did work: the same window **is** responsive to window-manager requests. `wmctrl -b
remove,fullscreen` followed by a resize to 1280x720 re-laid out the title at the new size, and
restoring 1920x1080 re-laid it out again - all three states still showing the replacement. So the
engine services `ConfigureNotify` while ignoring synthetic input, which also rules out "the process is
wedged and not reading its event queue" as the explanation.

**Resize survival, a positive result.** The 1280x720 capture (frame 749) and the restored 1920x1080
capture (frame 830) both show `HYPR` over `LODR` at the new layout, and the log contains a single
`title_mask_found`/`title_mask_loaded` pair and **no** `title_mask_stale` event. That means the texture
the mask was resolved against survived the resize, and the per-frame re-upload kept the replacement in
place across an engine-side layout change - the closest thing to a rebuild test available without
input.

## Delivery: the mask is installed, not hand-copied (2026-11-01)

Until now the replacement only worked because the glyph mask was copied into the game directory by
hand, which is not a reproducible pipeline. The mask is now part of what `tools/hyprl_install.py`
installs:

- `--title-mask PATH` places the mask as `hyprlodr-title-mask.raw`, records its SHA-256 in
  `hyprlodr-install-manifest.json`, and uninstall removes it - but only while the bytes still match,
  the same rule the mods follow;
- a mask that was already in the game directory is backupped into `mods/.hyprlodr-backup` and put back
  by uninstall, so installing never destroys a mask the user rendered earlier;
- the copy sits inside the install's rollback, so a failure later in the install leaves no mask behind;
- `--dry-run` prints which mask would be installed.

The mask itself is **not** committed. `/assets/hyprlodr-title-mask.raw` is ignored by Git because it is
rendered from a typeface that lives inside the user's own installation; `tools/make_title_mask.py
--font <face> --out assets/hyprlodr-title-mask.raw` reproduces it, and the geometry it targets is the
measured grid above.

**Verified in game** with the mask installed by this path rather than copied: the log records one
`title_mask_loaded` and one `title_mask_found`, and `HYPR` over `LODR` renders at frames 483 and 512.

## Recovering the face from the user's own installation (2026-11-01)

**Confirmed: every font slice in the container is a TrueType file behind a 24-byte header.** The
proxy's record log and the container agree to the byte: slice sizes are exactly *font size + 24*
(Georgia regular 219736 = 219712 + 24, Georgia bold 207500 = 207476 + 24, catalog up to Tinos regular
454876 = 454852 + 24), and the sfnt signature sits at offset 24 of every decrypted payload. This is
why an earlier scan that expected the signature at byte 0 found no fonts at all.

**Confirmed: the container ships eleven faces, and its own entry names say which.** The log's `key`
events list the 1008 entry names the resolver walks, among them `georgia`, `georgiab`, `georgiai`,
`georgiaz` and `goethebold`; the recovered `name` tables give them human names:

| Face | Slice | Family / style |
|---|---|---|
| `georgia` | 219736 | Georgia / Regular |
| `georgiab` | 207500 | Georgia / Bold |
| `georgiai` | 208468 | Georgia / Italic |
| `georgiaz` | 211804 | Georgia / Bold Italic |
| `goethebold` | 134416 | Goethe / Bold |
| - | 454876 / 419948 | Tinos Regular / Bold |
| - | 289076 | Mongolian Baiti Regular |
| - | 358280 / 309432 | Consolas Regular / Hack Regular |
| - | 191136 | Font Awesome 5 Free Solid |

**Confirmed: the pipeline is now reproducible from the repository plus the user's installation.**
`tools/title_mask_from_game.py` reads the record log, cracks each slice's 1024-key space, strips the
header, identifies faces by their `name` table and renders the words with `make_title_mask`. On this
installation it recovers all eleven faces, selects Georgia Bold, and produces a mask that is
**byte-identical** to the mask already verified in game - so the replacement no longer depends on a
face extracted by hand in an earlier session.

**Hypothesis, still unproven:** the original title lettering is one of the shipped high-contrast
serifs (Georgia Bold, Goethe Bold or Tinos Bold). All three render within a couple of pixels of each
other at the measured cap height, so the mask cannot distinguish them; proving which one the title
used needs the draw-time binding, not the geometry.
