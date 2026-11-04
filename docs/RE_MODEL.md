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
leaderboards,” not gameplay. The Steam minidump
`.tmp/asset-proof/run2/analysis/crash_steamuser.dmp` (SHA-256
`8ad63f3fc15b452ac30363d0c60e93fb1cac64e3d633bbf3f2d35766ded8018b`) records
access violation `0xc0000005`: a read from address `0x80` at
`hyperdemon.exe+0x75d7`. The faulting instruction is `CALL qword ptr [RAX+0x80]`
with `RAX=0`, consistent with a null object/vtable dispatch. Unwinding the
Windows x64 `.pdata` function ranges gives the game-thread chain
`FUN_1402ff6c0` → `FUN_140002980` → `FUN_1401c31a0` → `FUN_1401c33c0` →
`FUN_140073ca0` → `FUN_140006dd0`; the intermediate functions include the
main update loop. This places the exception in update processing, but does not
identify the object or subsystem that produced the null pointer. The crash
record does not prove a redirect wait or archive read was active at the fault.

The wait is a bounded spin with a 1 ms sleep and uses an acquire/release flag;
the resolver and wait logger do not call an intercepted file API (`WriteFile`
is used for logging), so the hook path is not recursively re-entered. The
bootstrap worker reads `hyprlodr-config.json`, configures the loader, scans the
live resolver tables, and resolves/initializes mods; the inspected native path
does not open `asset/asset` itself. It installs the file hook before resolving
mods, so later-loaded mod imports are not part of that initial hook pass. If
`CreateThread` fails, the proxy now marks the gate ready immediately: no worker
can publish a replacement, and the game falls back to its original archive
instead of waiting for the 65-second cap. A stuck worker still falls back at
the cap, as covered by the shortened-timeout fixture.

To separate startup redirection from archive contents, two controlled runs
used the same zero-delay configuration, current proxy, disabled rendering hook,
and first-open logging. Run 3 redirected to a separate byte-for-byte archive
copy (`.tmp/asset-proof/run3-original/asset.original-copy`, SHA-256 equal to the
installed archive, with a distinct inode). Its first `_Fiopen` of `asset/asset`
at 477 ms logged `redirected:true`, `ok:true`, and the control-copy path. Run 4
redirected to the already validated modified archive; its first open at 417 ms
also logged `redirected:true`, `ok:true`, and the patched path. Both processes
remained alive for more than one minute without a new minidump. The saved frame
at run 3 showed “Connected as [berds]”; the run 4 frame showed “Connecting to
leaderboards.” Neither run reached gameplay or confirmed loading/rendering of
`hand2`.

The original-copy baseline demonstrates that early redirection to a separate
archive copy does not by itself reproduce the prior startup crash. The modified
archive also did not reproduce it in its controlled run. Since the original
crash was not reproduced and the run conditions cannot establish which game
state differed, the cause remains unclassified; it is unsupported to attribute
it either to the early redirect or to modified model contents. Both runs prove
successful first archive opens only. Neither proves nor disproves that `hand2`
is the original first-person arm.

The redirect-gate Wine tests now cover an open before redirect publication, a
successful initialization with no archive redirect, initialization failure
after redirect registration (which rolls registrations back and releases the
open), and a deliberately shortened timeout (which falls back to the original
path while bootstrap continues). These stand-in tests verify wait/publication
ordering and fallback behavior, not game asset reads. The custom test process
entry point now calls `ExitProcess` after `main` returns, matching CRT process
termination so a lingering bootstrap thread does not keep Wine test clients
alive. No additional game launch was made.

Temporary game-directory log links and the test mod link were removed after
the attempt; the original `OpenAL32.dll` and prior logs were restored. The
installed `asset/asset`, `hyperdemon.exe`, and `OpenAL32.dll` SHA-256 values
after cleanup are respectively
`2ce9dabb3251cb8d7994e2bda74056d344c06bf55b0d47b8645029b4687f99c7` and
`7612dd72076002b7c0cb5cab8f7340a399176ecedefe9d3b4d0353fbdac93611` and
`8e0a069370f76de7084c39f23360311715f18677236d0e767c9af3f7992dbe64`.
Runtime artifacts and proprietary model/archive data remain in ignored `.tmp/`
and are not part of the source change.

## Named asset consumption trace

Ghidra analysis of the installed executable (SHA-256
`7612dd72076002b7c0cb5cab8f7340a399176ecedefe9d3b4d0353fbdac93611`) confirms
the following synchronous stages:

| Stage | Function and fields | Ghidra Evidence |
|---|---|---|
| Resolve `(kind, name)` | `FUN_140211e70`, RVA `0x211e70`; x64 arguments are manager, kind, MSVC `std::string*`, flags | `ev_74fedd06c73d04fef7948a33b620f678d9ad632564db3b010588d185c3801e61` |
| Select record and dispatch | `FUN_140212920`, RVA `0x212920`; record offset `+0`, encrypted size `+4`, type tag `+0x40`; seeks to the record offset | `ev_0ba001c861420fe99f102b252dee8d4b394d7afe4a30714983cbc8ee25604d4e` |
| Read and decrypt | `FUN_140213370`, RVA `0x213370`; reads into the supplied buffer then XOR-decrypts in place using the name-derived seed | `ev_927fde7c6e4d4ec5a20d94f0cb025789f7ea92cfe655a29876e74b2b0643aa52` |
| Parse tag 1 | `FUN_140205ec0`, RVA `0x205ec0`; checks graph version 3 and returns 1 on the v3 branch; loader passes the buffer at `+0x18` | `ev_8755e96a9525543eb66d79eb9ed029e52332bacf2003d0a5ef2479f2f6876af0` |
| Decode mesh arrays | `FUN_14021a710`, RVA `0x21a710`; counts at object `+0x0c/+0x10`, stride `+8`, array pointers `+0x20/+0x28`, completion marker 3 at `+0x4c` | `ev_652daa8d147c94a9ec2daef6ce8358a2143744c9edb7e5aaf1587d0598baedf2` |

The previous live resolver-table capture has two records named `hand2`: a
tag-2 record at offset `277743449`, size `87415`, and the tag-1 model at offset
`295758784`, size `21972` (`.tmp/asset-proof/run4-modified/runtime/hyprlodr-assets.jsonl`).
The resolver's `kind` argument selects the lookup table, so a name-only event
cannot identify the model. The tracer records both `kind` and the resolved
record. Its validator treats a tag-2 result as a separate same-name resource
and attributes model consumption only to the exact tag-1 offset, size, and type.
An offline comparison against the installed original archive and
`.tmp/asset-proof/asset.patched` confirms the decrypted payload hashes are
`ddfe1797efd29c1200e1c775d998b73ef73b9b0e8710e92d7b632c3a29c93fa8` and
`9b0e9cb0e446c56c481547c90994e707fd22f899e0a14e536eb039075328d795`
respectively. Both have 238 vertices, 1,188 indices, and 72-byte strides; the
modified vertex bytes differ while the index-array hash remains
`1af9f93e699537f502ffc48e81754d5f77b6f8f38ecc0da4a4214cd480832536`.

`tools/re/asset_trace_frida.js` uses Frida's reversible interceptors. It checks
the expected image base and five function-entry byte signatures before
installing hooks. The script performs no direct writes to game data. Detaching
removes its instrumentation. It emits separate events for name lookup, resolver
return, record selection, decrypted-payload hash, tag-1 parser result, and mesh
counts plus copied vertex/index hashes. Hashing is implemented in JavaScript
and checked against the standard SHA-256 `abc` test vector. The trace does not
infer a render from a successful decode. Nested non-target resolver calls push
an explicit null context so their reads cannot be attributed to an outer
`hand2` request.

The loader passes `pvVar3 + 0x18` to the graph parser but keeps the allocation's
full record size as its length argument. The nominal parser span is therefore
24 bytes larger than the readable payload view. The trace hashes at most
`record_size - 0x18` parser-input bytes and never reads beyond the allocation.
The decrypt function does not check the stream's `gcount`; the trace records
that limitation and treats an exact payload SHA-256 match as evidence that the
expected bytes were read and decrypted.

### Capture and compare

All capture output, reports, Python packages, and temporary files belong under
the ignored persistent `.tmp/` directory. If Python Frida is not installed,
keep it local to the repository:

```bash
mkdir -p .tmp/pydeps .tmp/pycache .tmp/logs
export TMPDIR="$PWD/.tmp" TMP="$PWD/.tmp" TEMP="$PWD/.tmp"
export PYTHONPYCACHEPREFIX="$PWD/.tmp/pycache"
export PYTHONPATH="$PWD/.tmp/pydeps"
python3 -m pip install --target .tmp/pydeps frida
```

For a running process, `capture --pid` and `--wait-process` are late-attach
diagnostics. The report explicitly marks `process_start_coverage: false`; an
empty trace from those modes cannot establish that no earlier request occurred.
When Frida spawn gating is available, the launch mode installs every hook before
resuming the game process:

```bash
python3 tools/re/asset_trace.py capture \
  --launch-proton-child --wait-seconds 45 --duration 90 \
  --out .tmp/logs/hand2-modified-trace.jsonl
```

This host has `kernel.yama.ptrace_scope=1`. A local Frida child-process fixture
attached successfully, showing that ordinary child tracing is allowed. Frida
device-wide spawn gating, however, returns `PermissionDeniedError: operation
not permitted`. The capture tool records this failure before it launches the
game. The direct-child attach check is preserved in
`.tmp/logs/frida-child-attach-fixture.json`. The prior manual Proton child
attempt also did not expose a game process; its launcher log was empty. No
instrumented HYPER DEMON run has yet been made,
so there is currently no runtime evidence that `hand2` was requested, read,
decrypted, parsed, or rendered. The earlier redirected archive-open event is
not per-record evidence. The current denial is preserved in
`.tmp/logs/actual-spawn-gating-check.jsonl` and its `.launcher.json` companion;
the offline original-versus-modified hash comparison is in
`.tmp/analysis/hand2-trace-no-runtime-report.json`.

On a host where spawn gating is allowed, the mode records process selection,
attach, signature checks, completed hook installation, and the beginning of
coverage separately. It resumes the game only after `trace_ready` confirms the
five hooks are installed. Unrelated spawned processes are resumed immediately.
If the host denies gating, do not substitute an empty late-attach capture as
proof that an asset was not loaded. Compare the capture with both archive
variants only after instrumentation is active:

```bash
python3 tools/re/asset_trace.py validate \
  --events .tmp/logs/hand2-modified-trace.jsonl \
  --modified-pack .tmp/asset-proof/asset.patched \
  --report .tmp/logs/hand2-modified-report.json
```

An exact `payload_decrypted` hash identifies which payload reached the
decryption output. Matching array hashes at `mesh_decode`, with marker 3 and
the expected counts and stride, proves that the parser copied the edited
geometry into the game's mesh object. This still does not prove a graphics draw
used that object. A lookup or record event without later stages is reported at
that stage only. A no-request result is interpreted according to process
identity, attach, signature validation, hook installation, and coverage start;
late-attach capture results explicitly state that earlier asset activity may
have been missed. Rendering remains unobserved unless separately instrumented.

## Passive GPU hand capture and comparison

`examples/hand_hook.c` 0.9.4 adds an opt-in pass-through capture at the verified
first-person hand draw. The implementation records actual vertex and index bytes
from OpenGL buffers after validating the six attribute bindings and buffer bounds;
the original draw is forwarded unchanged. The `HYPRCAP1` fixed-header format and
`tools/re/hand_capture.py` comparison workflow are documented in `RE_HAND.md`.

Synthetic tests prove the parser distinguishes exact source bytes, global affine
position transforms, unique vertex reordering, ambiguous attribute keys, and malformed
captures. Runtime comparison is still unresolved: the latest capture attempt installed
the draw hook, but the installed proxy did not redirect the first archive open to the
modified archive, and the game exited before gameplay. No GPU buffer capture exists
from that attempt. The `hand2` source identity therefore remains supported by matching
dimensions and attribute layout, but has not been confirmed by captured geometry.
