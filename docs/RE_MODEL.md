# HYPER DEMON model payloads (initial findings)

Status: one tag-1 geometry section is decoded and exported to Wavefront OBJ. The
original first-person arm's source record is **not yet confirmed**.

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

Those matching dimensions and the logical name make this a strong source-asset
candidate for the first-person arm. The live VBO bytes were not available here
for a vertex-by-vertex comparison, so the source identity is still unconfirmed.
The four-float attribute's runtime meaning and the graph's following named
properties/material references also remain to be decoded.

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
  --json .tmp/analysis/hand2-model.json --obj .tmp/extracted/hand2.obj
blender -b --python-expr 'import bpy; bpy.ops.wm.read_factory_settings(use_empty=True); bpy.ops.wm.obj_import(filepath=".tmp/extracted/hand2.obj"); bpy.ops.wm.save_as_mainfile(filepath=".tmp/extracted/hand2.blend")'
```

The `asset_model.py` inspector currently recognizes the version-3 tag-1 mesh
section and writes positions, normals, UV0, and indexed faces to OBJ. Unknown
tail properties remain in the original decrypted payload and are not guessed or
discarded. The tool rejects truncated sections, non-finite core vertex values,
and out-of-range indices. Extracted payloads and generated OBJ/Blend files are
local game content and must not be committed.
