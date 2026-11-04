# HYPER DEMON model payloads (initial findings)

Status: version-3 tag-1 geometry is decoded with its graph-level name/value
prefix preserved. OBJ and a Blender script exporter are available. The identity
of `hand2` as the original first-person arm source is **strongly supported but
not byte-verified**.

## Confirmed tag-1 mesh section

Ghidra decompilation of `FUN_140205ec0` (RVA `0x205ec0`) shows a version-3 graph
parser. It reads the graph version and a length-delimited binary section, then
parses named properties and object references after that section. The section
decoder `FUN_14021a710` (RVA `0x21a710`) reads an index count, vertex count, and
one-byte vertex stride; it copies `stride * vertex_count` bytes followed by
`index_count * 4` bytes. These sizes are bounds-checked by the offline parser.

For the locally installed build, the verified resolver mapping
`tbl-e91fb3b80004:hand2` identifies tag `1`, record
`rec-011a0ebc0-000055d4-0001` (archive offset `295758784`, size `21972`). After
decryption, the payload has:

| Field | Value | Evidence |
|---|---:|---|
| graph version | 3 | `FUN_140205ec0`; payload offset `0x18` |
| section size | 21,902 bytes | payload offset `0x1c` |
| index count | 1,188 | `FUN_14021a710`; section offset `+4` |
| vertex count | 238 | `FUN_14021a710`; section offset `+8` |
| vertex stride | 72 bytes | `FUN_14021a710`; section offset `+12` |
| vertex array | payload offset 46, length 17,136 | calculated from decoded count and stride |
| index array | payload offset 17,182, length 4,752, u32 | decoder copies four bytes per index |

All 238 positions and normals are finite; normal lengths are 0.99994–1.00008.
All 1,188 indices address vertices 0–237. This payload therefore contains a
valid indexed triangle mesh. The 72-byte stride agrees with the independently
measured live hand buffer layout in `RE_HAND.md`: position at 0, normal at 12,
UV at 24, a four-float attribute at 32, tangent at 48, and bitangent at 60.
The counts also match that live draw exactly (238 vertices and 1,188 indices).

The live draw evidence independently reports the same 238 vertices, 72-byte
stride, and 1,188 indices. Its attribute offsets are 0/12/24/32/48/60, matching
the six 32-bit-float groups in this archive record. Existing runtime analysis
identifies the four-float group at byte 32 as UV/mask data (its x values vary
0.0051–0.0668, y is constant 0.0922, z/w are zero), not skinning weights. This
supports a direct source relationship, but the persistent VBO/IBO byte dumps
mentioned in earlier notes are no longer present in the repository's `.tmp/`
tree or the available persistent hand-extract directories. No vertex-by-vertex
position, normal, UV, or topology comparison can therefore be reproduced from
the available captures. The live GPU geometry is posed; equality may also
require accounting for runtime coordinate transforms or deformation.

### Vertex attributes and graph tail

The archive decoder `FUN_14021a710` copies the vertex buffer as opaque
`vertex_count * stride` bytes; it does not assign semantics to individual
fields. The offsets below combine the archive record layout with the independent
OpenGL attribute-pointer capture (`docs/evidence/2026-11-01-hand-draw.json`):

| Offset | Components | Current description | Evidence/status |
|---:|---:|---|---|
| 0 | 3 float32 | position | OpenGL location 0; finite values |
| 12 | 3 float32 | normal | OpenGL location 1; lengths near 1 |
| 24 | 2 float32 | UV0 | OpenGL location 2 |
| 32 | 4 float32 | UV/mask-like attribute | OpenGL location 3; runtime range and prior shader analysis; not bone weights |
| 48 | 3 float32 | tangent | OpenGL location 4; vector semantics supported by existing shader analysis |
| 60 | 3 float32 | bitangent | OpenGL location 5; vector semantics supported by existing shader analysis |

For `hand2`, the graph tail begins at payload offset 21,934 and has 38 bytes.
Ghidra `FUN_140205ec0` reads a u32 entry count, then each entry as a u8-length
name, eight raw value bytes, and two u16 fields. The decoded entry is
`Group8673`, raw value `00000000a4040000`, fields `0xffff` and `0`. The following
u32 reference count is zero; eight zero bytes remain. `hand2left` similarly has
entry `handlow` (`00000000be0b0000`, fields `0xffff, 0`), and `handbones` has
`boneslow.001` (`000000008c130000`, fields `0xffff, 0`); each reports zero
references and eight trailing zero bytes. In all three raw values, the upper
little-endian u32 equals the record's index count (1,188 / 3,006 / 5,004).
This repeatable correlation is recorded as raw data, not a decoded semantic.
The name may identify a graph child/group, but the owning loader consumer has
not been traced far enough to establish that. These entries do not establish a
material assignment; no material reference is present in the parsed reference
list.

`asset_model.py` reports per-component ranges for all six attribute groups and
preserves unknown graph-tail bytes. Its `--blender-script` output builds the
indexed mesh, UV0 layer, and point attributes for normal, the four-float
channel, tangent, and bitangent. The Blender file is generated locally; no game
payload or extracted content is committed. Blender has no material assignment
to export from these graph records because none is verified.

## Lossless rebuild and same-count editing

The payload parser now also acts as a conservative serializer. It validates the
version, section bounds, counts, stride, vertex/index array extents, finite
positions/normals/UV0, and index bounds. A no-edit rebuild returns the original
payload bytes unchanged. An edited rebuild copies that payload and changes only
the explicitly supplied float32 vertex components at their original offsets.
The header, binary section prefix, vertex padding, indices, graph entry names
and values, references, unknown bytes, and trailing data all remain byte-for-
byte from the source. A rebuild requires the version-3 entry and reference-count
prefix to parse within bounds; uninterpreted bytes after that prefix are
retained. Output cannot overwrite its source payload.

The editing interface accepts a JSON document with the source `vertex_count`
and arrays under `attributes`; supported names are `position` (3), `normal`
(3), `uv0` (2), `attribute3` (4), `tangent` (3), and `bitangent` (3). Each
array must contain exactly one finite row per source vertex. The serializer
rejects unknown attributes, changed vertex counts, wrong row widths, non-finite
values, invalid float32 ranges, and any rebuilt payload that fails structural
validation. Topology and index edits are intentionally unsupported. Blender
export also rejects UV seams that would require duplicating a vertex, since
vertex-count changes are outside this format-safe path.

The graph-tail values remain uninterpreted. In the verified hand records, the
upper little-endian u32 of the raw eight-byte named-entry value equals the
record's index count. The rebuilt edit path keeps both mesh counts fixed and
preserves these graph bytes unchanged; this avoids violating that observed
relationship. No material or independent geometry-reference format has been
confirmed.

### Reproduce extraction, no-change round trip, and edit

Set persistent scratch paths before running Python or Blender:

```bash
mkdir -p .tmp/extracted .tmp/analysis .tmp/logs .tmp/roundtrip
export TMPDIR="$PWD/.tmp" TMP="$PWD/.tmp" TEMP="$PWD/.tmp"
GAME="$HOME/.local/share/Steam/steamapps/common/hyperdemon"
```

Extract `hand2` and prove the no-change serializer is byte-identical:

```bash
python3 tools/re/hyprl_assets.py extract 'tbl-e91fb3b80004:hand2' \
  --pack "$GAME/asset/asset" --exe "$GAME/hyperdemon.exe" \
  --evidence "$GAME/hyprlodr-assets.jsonl" \
  --index .tmp/analysis/assets-index.json --out .tmp/roundtrip/hand2.bin
python3 tools/re/asset_model.py .tmp/roundtrip/hand2.bin \
  --out .tmp/roundtrip/hand2-rebuilt.bin
cmp .tmp/roundtrip/hand2.bin .tmp/roundtrip/hand2-rebuilt.bin
```

Create the attribute-preserving Blender mesh, edit it while keeping its vertex
count and per-vertex UV layout, then export an edit document:

```bash
python3 tools/re/asset_model.py .tmp/roundtrip/hand2.bin \
  --blender-script .tmp/analysis/create-hand2.py \
  --blend-output .tmp/extracted/hand2.blend
blender --background --python .tmp/analysis/create-hand2.py
# Edit and save .tmp/extracted/hand2.blend in Blender as hand2-edited.blend.
python3 tools/re/asset_model.py .tmp/roundtrip/hand2.bin \
  --blender-export-script .tmp/analysis/export-hand2.py \
  --blend-input .tmp/extracted/hand2-edited.blend \
  --edit-json-output .tmp/analysis/hand2-edits.json
blender --background .tmp/extracted/hand2-edited.blend \
  --python .tmp/analysis/export-hand2.py
python3 tools/re/asset_model.py .tmp/roundtrip/hand2.bin \
  --edit-json .tmp/analysis/hand2-edits.json \
  --out .tmp/roundtrip/hand2-edited.bin
```

Encrypt the rebuilt payload to a separate local file; then verify it through the
existing cipher library. The installed pack is read only:

```bash
python3 tools/re/pack_crypt.py --exe "$GAME/hyperdemon.exe" \
  --pack "$GAME/asset/asset" --name hand2 --encrypt \
  --in .tmp/roundtrip/hand2-edited.bin \
  --out .tmp/roundtrip/hand2-edited.enc
python3 - <<'PY'
from pathlib import Path
from tools.re.pack_crypt import decrypt, load_tables, seed_for_name
game = Path.home() / ".local/share/Steam/steamapps/common/hyperdemon"
t0, t1, _ = load_tables(game / "hyperdemon.exe")
expected = Path(".tmp/roundtrip/hand2-edited.bin").read_bytes()
cipher = Path(".tmp/roundtrip/hand2-edited.enc").read_bytes()
assert decrypt(cipher, t0, t1, seed_for_name("hand2")) == expected
print("encrypted payload decodes byte-identically")
PY
```

### Validation performed

Freshly extracted `hand2`, `hand2left`, `handbones`, `handbonesleft`, and
`handveins` all rebuilt byte-for-byte. For `hand2`, source and rebuild SHA-256
were both `ddfe1797efd29c1200e1c775d998b73ef73b9b0e8710e92d7b632c3a29c93fa8`.
The controlled Blender edit moved vertex 0 by `+0.001` on X: only the three
float32 bytes at payload offsets 46–48 changed. The edited model retained 238
vertices, 1,188 indices, and 72-byte stride. Encrypting that local payload with
the installed executable's tables and `hand2` seed, then decrypting it, returned
the edited bytes exactly. All generated payloads, Blender files, ciphertext,
edit JSON, and logs remain under ignored `.tmp/`.

### Source-arm identity assessment

The record is a strong candidate for the original first-person arm: its logical
name is `hand2`, its geometry dimensions match the draw exactly, its stride and
attribute offsets match, and its bounds are arm-scale. Matching counts do not
prove identity. The decisive per-vertex capture bytes are unavailable, and the
runtime draw may apply a transform or a CPU-side pose before upload. Current
evidence supports but does not confirm `hand2` as the source mesh. Recovering a
new live capture is unnecessary for this offline milestone and was not
attempted.

## Reproduce locally

Keep outputs and tool temporary files under the ignored, persistent repository
directory `.tmp/`:

```bash
mkdir -p .tmp/extracted .tmp/analysis .tmp/logs
export TMPDIR="$PWD/.tmp" TMP="$PWD/.tmp" TEMP="$PWD/.tmp"
GAME="$HOME/.local/share/Steam/steamapps/common/hyperdemon"

python3 tools/re/hyprl_assets.py extract 'tbl-e91fb3b80004:hand2' \
  --pack "$GAME/asset/asset" --exe "$GAME/hyperdemon.exe" \
  --evidence "$GAME/hyprlodr-assets.jsonl" \
  --index .tmp/analysis/assets-index.json --out .tmp/extracted/hand2.bin
python3 tools/re/asset_model.py .tmp/extracted/hand2.bin \
  --json .tmp/analysis/hand2-model.json --obj .tmp/extracted/hand2.obj \
  --blender-script .tmp/analysis/hand2-blender.py \
  --blend-output .tmp/extracted/hand2-full.blend
blender --background --python .tmp/analysis/hand2-blender.py
blender -b --python-expr 'import bpy; bpy.ops.wm.read_factory_settings(use_empty=True); bpy.ops.wm.obj_import(filepath=".tmp/extracted/hand2.obj"); bpy.ops.wm.save_as_mainfile(filepath=".tmp/extracted/hand2.blend")'
```

The `asset_model.py` inspector recognizes the version-3 tag-1 geometry section,
reports vertex attribute ranges and a bounded decode of graph names/raw values,
and retains uninterpreted graph-tail bytes in JSON. OBJ remains a surface-only
export; the Blender script export preserves the known non-position attributes.
The tool rejects truncated sections, non-finite positions/normals/UV0, and
out-of-range indices. Extracted payloads and generated OBJ/Blend files are local
game content and must not be committed.

## Native replacement experiment (2026-11-03)

A local proof archive was built under `.tmp/asset-proof/` by scaling every
`hand2` position by 1.35 through the existing Blender edit/export path. It
retains 238 vertices, 1,188 indices, the 72-byte vertex stride, and the native
payload length. The encrypted patch was checked against the original archive:
the archive size is unchanged, bytes outside the `hand2` record are identical,
and decrypting that record with the installed executable's tables and the
`hand2` key returns the edited payload. The installed archive was not changed.

The first runtime attempt exposed a startup race: the first `asset/asset` open
at `t_ms: 296` via `_Fiopen` had `redirected: false`, while mod initialization
completed only after the default 2500 ms delay. Setting `delay_ms: 0` alone is
not a reliable ordering mechanism because worker scheduling still races the
game thread.

`native/win32/file_hook.c` now waits only when an intercepted path ends in
`asset/asset` (case-insensitive, slash-normalized). It waits for the bootstrap
worker to publish the redirect registry or finish/fail initialization, with a
65-second bound. It does not run mod code in `DllMain`; hook installation is
early and mod discovery/initialization stays on the worker. The stand-in host
regression test opens `asset/asset` before the mod registers its redirect. With
the default delay it waited about 1.5 seconds, then selected the replacement
and read its known bytes; with `delay_ms: 0`, the same open also selected the
replacement. This checks ordering, path selection, and readable replacement
content in the stand-in process.

One controlled game attempt used the existing 35%-enlarged archive copy at
`.tmp/asset-proof/asset.patched`. Its first archive event was at `t_ms: 342`
via `_Fiopen`, with `redirected: true`, `ok: true`, and the opened path pointing
to that patched archive. Here `ok` means `_Fiopen` returned a stream; this log
does not instrument individual reads or prove that the changed `hand2` bytes
were decoded or rendered. The runtime resolver capture includes the `hand2`
tag-1 record at offset `295758784`, size `21972`, and the asset probe completed
its 624 mapping capture. Those facts establish that the redirected archive was
opened and resolver metadata was collected, but do not establish the model's
use by the first-person arm.

The captured frame at
`.tmp/asset-proof/run2/analysis/game-frame-120.png` shows “Connecting to
leaderboards,” not gameplay. The game then raised access violation
`0xc0000005` at `hyperdemon.exe+0x75d7`, from the Steam minidump. This confirms
the run stopped before the requested visual check; the available evidence does
not attribute the crash to the replacement archive or to the loader. Therefore
the run proves first-open redirection but neither proves nor disproves that
`hand2` is the original first-person arm. No follow-up game launch was made.

Temporary game-directory log links and the test mod link were removed after
the attempt; the original `OpenAL32.dll` and prior logs were restored. The
installed `asset/asset`, `hyperdemon.exe`, and `OpenAL32.dll` SHA-256 values
after cleanup are respectively
`2ce9dabb3251cb8d7994e2bda74056d344c06bf55b0d47b8645029b4687f99c7` and
`7612dd72076002b7c0cb5cab8f7340a399176ecedefe9d3b4d0353fbdac93611` and
`8e0a069370f76de7084c39f23360311715f18677236d0e767c9af3f7992dbe64`.
Runtime artifacts and proprietary model/archive data remain in ignored `.tmp/`
and are not part of the source change.
