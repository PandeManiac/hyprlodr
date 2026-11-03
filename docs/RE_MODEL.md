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
