# HYPER DEMON model payloads and topology-aware rebuilding

Status: version-3 tag-1 geometry is decoded and can be rebuilt losslessly or
with validated topology edits. A passive OpenGL capture matches the edited
tag-1 `hand2` vertex and index buffers byte-for-byte at the recognized hand
draw; see the capture evidence in `RE_HAND.md`.

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
the six 32-bit-float groups in this archive record. The retained raw capture
and exact edited-payload comparison are described in `RE_HAND.md`: the captured
draw bytes match the deliberately enlarged `hand2` payload exactly. The
four-float group at byte 32 varies like UV/mask data in the capture and is not
verified as skinning weights.

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
The name may identify a graph child/group, but the eight-byte value's semantic
consumer remains unresolved. These entries do not establish a material
assignment; no material reference is present in the parsed reference list.

`asset_model.py` reports per-component ranges for all six attribute groups and
preserves unknown graph-tail bytes. Its `--blender-script` output builds the
indexed mesh, UV0 layer, the point attributes for normal, the four-float
channel, tangent, and bitangent, plus a `hyprlodr_source_vertex` template index.
The Blender file is generated locally; no game payload or extracted content is
committed. Blender has no material assignment to export from these graph
records because none is verified.

## Lossless rebuild and same-count editing

The payload parser also acts as a conservative serializer. It validates the
version, section bounds, counts, stride, vertex/index array extents, finite
known vertex fields, non-degenerate triangle indices, and index bounds. A
no-edit rebuild returns the original payload bytes unchanged. A rebuild
requires the version-3 entry and reference-count prefix to parse within bounds;
uninterpreted bytes after that prefix are retained. Output cannot overwrite its
source payload.

The editing interface accepts legacy same-count JSON edits under `attributes`
and a topology-aware form with `vertex_count`, `source_vertices`, `indices`, and
all six `attributes` arrays. Supported attributes are `position` (3), `normal`
(3), `uv0` (2), `attribute3` (4), `tangent` (3), and `bitangent` (3). A
topology edit selects a source vertex record for every output vertex, copies
that complete 72-byte record (including any unsupported bytes), then replaces
the known float groups. This makes duplication and vertex reordering explicit;
brand-new vertices need a valid source template. Indices are u32 triangle
indices, must be in bounds, have a count divisible by three, and may not repeat
a vertex within one triangle. All supported float values must be finite and
representable as float32. The serializer rebuilds the mesh section count and
length fields and relocates the original graph tail and following bytes
unchanged. Declared section padding is retained after the new arrays. No-edit
rebuilds remain byte-identical.

The Blender exporter now writes triangle indices as well as vertex attributes.
It splits output vertices when one Blender vertex has multiple `uv0` corner
values, preserving UV seams by duplicating that vertex and its selected source
template. It rejects non-triangle polygons; triangulate intentionally before
export. Each newly created Blender point must have a valid integer
`hyprlodr_source_vertex` value selecting the source record whose unsupported
bytes should be copied. New positions/attributes are serialized, while unknown
record bytes come from that chosen template.

The graph-tail eight-byte values remain uninterpreted. Across `hand2`,
`hand2left`, and `handbones`, the upper little-endian u32 equals the source
index count, but that correlation has not established a draw-range or count
meaning. Ghidra evidence for the shipped executable (SHA-256
`7612dd72076002b7c0cb5cab8f7340a399176ecedefe9d3b4d0353fbdac93611`) shows
`FUN_140205ec0` copies the raw 8-byte value into graph-node storage at `+0x40`
and `+0x44` and the nearby `FUN_140206870`/`FUN_140206900` handlers construct
and insert name-keyed tree nodes; `FUN_140207280` performs another name-keyed
lookup/insertion. The inspected functions do not establish a consumer that
interprets this value as a mesh index count. Evidence IDs: parser
`ev_8755e96a9525543eb66d79eb9ed029e52332bacf2003d0a5ef2479f2f6876af0`, node
creation `ev_1979e41bb539ffd8a56c1de8bd39a9b3625dac046ed0a96dab4dc7949e1f382b`,
tree insertion `ev_656123a075e47ea3b037ea2adb7e72df2a97b6e11b31a5e3dba4d767a7cddc07`,
and lookup `ev_f68695fcb0102cc96b9560a9a32e96bb930f0ca6c3ecbf2ea1f2734dd9f675d8`.
Accordingly topology edits preserve those eight bytes exactly rather than
rewriting a field whose meaning is not verified. This remains a possible runtime
compatibility risk to resolve before shipping edited assets. No material or
independent geometry-reference format has been confirmed.

### Topology changes and variable-size delivery

The serializer recalculates the tag-1 v3 mesh section size and its index and
vertex counts; these are the only dependent binary fields with confirmed
semantics. It retains the section's four-byte prefix, stride, pad byte, section
padding, all original bytes after the section, and graph values. The current
tool has operational limits of 2,000,000 vertices and 12,000,000 indices to
bound its in-memory rebuild buffers; these are tool limits, not asserted game
format maxima. The loader casts record size to signed 32-bit for `malloc`, so a
serialized payload also must remain below 2 GiB. The current
loader's record metadata has a u32 archive offset at `+0`, u32 record size at
`+4`, and type at `+0x40` (`RE_PACK.md`). `FUN_140212920` allocates the record
size, seeks to that offset, reads and decrypts the same size, dispatches the
plaintext and size to the type parser, then frees the temporary read buffer
(Evidence `ev_0ba001c861420fe99f102b252dee8d4b394d7afe4a30714983cbc8ee25604d4e`).
`FUN_14021a710` copies the mesh arrays into a separate parsed mesh object
(Evidence `ev_652daa8d147c94a9ec2daef6ce8358a2143744c9edb7e5aaf1587d0598baedf2`).

#### Append-only variable-size delivery prototype

`tools/re/pack_append.py` now builds an append-only archive artifact for a
variable-size, encrypted tag-1 model. It requires an asset index bound to the
current executable and archive hashes, a resolver-verified record ID and table
ID, the exact original `(offset, size, tag)`, and the SHA-256 of the original
ciphertext. It rejects ambiguous name/table matches and the tag-2 `hand2` record.
The replacement is parsed as a v3 tag-1 mesh, encrypted with the logical name's
seed, and appended at exact EOF. No alignment rule has been demonstrated: the
reader uses a seek to the record offset and requests the record size, so the
prototype does not insert padding. The generated archive is checked to have the
original archive as an exact byte-for-byte prefix. A JSON manifest records both
fingerprints, old and new record metadata, ciphertext/plaintext hashes, the key
class, and decoded mesh counts.

Reproduction for the retained local 239-vertex / 1,191-index `hand2` payload
(paths below are ignored local artifacts; use this installed build only):

```bash
mkdir -p .tmp/archive-tests
export TMPDIR="$PWD/.tmp" TMP="$PWD/.tmp" TEMP="$PWD/.tmp"
python3 tools/re/pack_append.py \
  --exe "$GAME/hyperdemon.exe" --pack "$GAME/asset/asset" \
  --index .tmp/analysis/assets-index.json \
  --payload .tmp/roundtrip/hand2-blender-added.bin \
  --out .tmp/archive-tests/asset-hand2-appended \
  --manifest .tmp/archive-tests/hand2-override.json \
  --name hand2 --record-id rec-011a0ebc0-000055d4-0001 \
  --table-id tbl-e91fb3b80004 --offset 295758784 --size 21972 --tag 1 \
  --expected-pack-sha256 2ce9dabb3251cb8d7994e2bda74056d344c06bf55b0d47b8645029b4687f99c7 \
  --expected-exe-sha256 7612dd72076002b7c0cb5cab8f7340a399176ecedefe9d3b4d0353fbdac93611 \
  --expected-record-cipher-sha256 2795c3304d81d7504a7af300628f141bc702dd727f36b62d557318bf76e9874a
```

For that artifact, the source archive is 306,794,369 bytes; the appended
ciphertext begins at offset 306,794,369 and is 22,056 bytes. Offline validation
confirmed the exact original prefix, decrypt/re-encrypt equality with the
serialized plaintext, and parsed counts 239 vertices / 1,191 indices at stride
72. These results demonstrate that the payload and appended archive are
internally consistent; they do not show that the game follows the new offset.
The existing same-size `pack_patch.py` remains unchanged and continues to serve
in-place record replacement.

No live record override is shipped. Ghidra confirms `FUN_140212920` reads the
u32 offset at descriptor `+0`, u32 size at `+4`, allocates `(int)size`, seeks,
reads/decrypts that exact size, dispatches the buffer, and frees the temporary
read buffer. The type tag is at `+0x40`; the record has 0x58-byte stride and
trailing cached parsed-object state. `FUN_14021a710` copies mesh arrays into a
separate parsed mesh object, so the ciphertext buffer lifetime ends after
parsing. The append reader path is therefore compatible with the observed
offset/size mechanism, but the archive alone cannot direct the game to EOF:
runtime resolver records must be changed.

#### Loader routing boundary investigation

The exact-build Ghidra view of `FUN_140211e70` (RVA `0x211e70`) shows the
resolver copying a 0x58-byte record into a stack-local descriptor with
`FUN_140212040`, then calling `FUN_140212920` at RVA `0x211f78`. This is the
only direct callsite found for the record loader. Its descriptor fields used
here are offset `+0`, size `+4`, type tag `+0x40`, and parsed-object pointer
`+0x48`. The tag-1 branch checks/initializes the parsed-object pointer, reads
and decrypts the requested extent, passes plaintext to `FUN_140205ec0`, and
the caller then copies the local descriptor into its name-keyed loaded-record
cache. `FUN_14021a710` copies mesh arrays into separately owned parsed storage;
the temporary encrypted/decrypted allocation is freed after parsing.

This establishes a safer substitution *operation* than editing a resolver
table: change only the stack descriptor's offset and size immediately around
the original loader call, then restore those two words before returning to
`FUN_140211e70`. The loader's parsed-object pointer at `+0x48` must remain
untouched by restoration so the resolver's normal cache copy can retain the
object it just constructed. The original resolver record and the cached
offset/size fields are then left at their original values. The pair is coherent
for the duration of the call because both fields are substituted before it and
restored only after it returns.

### Exact loader call instruction (installed executable, 2026-11-04)

The installed `hyperdemon.exe` was freshly hashed as
`7612dd72076002b7c0cb5cab8f7340a399176ecedefe9d3b4d0353fbdac93611`, matching
the pinned build. `llvm-objdump` disassembly at RVA `0x211f78` gives:

```text
140211f71  4c 8d 55 c7       lea  rdx, [rbp - 0x39] ; request-local 0x58 record
140211f75  49 8b cd          mov  rcx, r13         ; ctx
140211f78  e8 a3 09 00 00    call 0x140212920      ; FUN_140212920
140211f7d  48 8d 55 6f       lea  rdx, [rbp + 0x6f] ; cache key setup follows
```

This is a five-byte direct `call rel32`; the Windows x64 ABI arguments are
`RCX=ctx`, `RDX=record`, `R8=name`, and `R9D=flags`. The local record is
materialized immediately before the call and the caller consumes its parsed
object pointer after return when it copies the record into the name cache. The
five call bytes are now an additional build anchor (`call:record_loader`), so
the fingerprint also rejects a changed call instruction.

The executable imports `OpenAL32.dll` directly (PE import descriptor name at
RVA `0x3cfcc0`) and has no delay-import directory. Its TLS data directory is
present, but the callback pointer array at RVA `0x3153a0` begins with a null
pointer. The process image is already mapped when an imported DLL receives
`DLL_PROCESS_ATTACH`; Windows documents `reserved != NULL` for process
initialization and `reserved == NULL` for a dynamic `LoadLibrary` attach. This
lets the proxy reject late loads rather than trying to infer timing from a
thread count. The Windows x64 loader keeps process initialization ahead of the
main executable entrypoint; newly created threads cannot finish their DLL
initialization while the loader lock is held. Wine exhibits the same attach
argument distinction in the compiled fixture.

HYPRLODR now supports an explicit `HYPRLODR_EARLY_RECORD_HOOK=1` launch
opt-in. Before writing the callsite, DllMain checks the exact timestamp/image
size, all nine in-memory anchors, the complete five call bytes and decoded
original target, confirms a static OpenAL32 import, and rejects a nonempty main
image TLS callback list. It performs no manifest/file hashing, JSON parsing,
mod loading, or wait in this path. The hook installer uses a bounded 64-address
near-relay probe, then VirtualAlloc/VirtualProtect/FlushInstructionCache; any
failure leaves the original callsite intact. The original loader pointer and
null-route binding are published before the patch. The hook starts disabled,
so calls before manifest validation transparently invoke the original loader
once.

This supports a pre-entry installation point for the pinned executable under
Windows loader initialization and the corresponding Wine fixture. It does not
prove that dependency DLL initialization code cannot synchronously call into
private game code, nor does `reserved != NULL` alone prove the process has no
externally created thread. The executable's empty TLS callback list and static
import establish the ordinary loader path, while dynamic proxy loads are
explicitly refused. There is no fallback late patch.

The generic Windows facility is implemented in
`native/win32/callsite_hook.c` and is installed only when
`HYPRLODR_EARLY_RECORD_HOOK=1` is present during a verified process-start
attach. Its supported mode requires the caller's quiescence assertion and
retains relay/callback state until process exit; logical disablement never
rewrites the instruction. `record_route_hook.c` adapts the verified MSVC string
object to the route helper without treating the object pointer as a C string.
The Wine fixture exercises dormant forwarding and deferred routing through an
actual emitted call instruction. This is synthetic evidence, not a real game
interception.

The proxy config accepts an opt-in `"experimental_record_route": true` flag
plus `experimental_record_route_manifest` and
`experimental_record_route_archive` paths. The new strict Windows validator is
in `native/win32/route_manifest.c`. It accepts schema 1 and the exact
`append-only-record-override-required` delivery, checks the hard-coded game
SHA-256 as well as the on-disk executable, checks the archive's full SHA-256
and source-prefix SHA-256, and verifies both the original record ciphertext
and appended replacement ciphertext. It rejects duplicate JSON keys, escaped
strings, non-integer/overflow values, trailing commas, excessive nesting,
missing required fields, unsupported schema/delivery, wrong resolver table,
record ID, original extent or tag, and non-append replacement ranges. Hashing
uses bounded-memory Windows CNG streams during the bootstrap worker, not in
the resource-loader or render callbacks.

On successful preflight, the route and its `hand2` name are stored in static
proxy storage. The route's name pointer refers to its owner, and the hook
binding points to the static callsite state and route for process lifetime.
The binding pointer is published and read with Windows interlocked pointer
operations, so the early installer publishes callback state before changing
the callsite and the worker publishes the route before enabling it.
Failures free only temporary parser storage; no published route is reclaimed.
`hyprlodr-attach.json` reports `experimental_record_route_requested` and a
specific preflight status. A successful result is reported as
`manifest_validated_waiting_for_redirect`: when a dormant early hook exists,
this means the inputs are validated but routing remains disabled. Otherwise a
valid route reports `validated_early_hook_missing`; malformed or stale inputs
retain the validator-specific failure status.

The Wine fixture executes this compiled validator against the real 306,816,425
byte appended archive and the installed executable. It also exercises stale
executable/archive identities, modified ciphertext, wrong original metadata,
wrong table and tag 2 with the same name, a truncated archive, missing fields,
unsupported schema, u32 overflow, and parallel reads of immutable route state.
This covers archive integrity and resource identity; it deliberately does not
duplicate the native model parser. Offline decryption using the captured
runtime cipher tables independently confirms a 22,056-byte plaintext with 239
vertices, 1,191 indices and stride 72. The original 306,794,369-byte archive
is the exact prefix declared by the manifest; neither installed game file was
modified.

### Resolver/cache/stream concurrency status

Fresh disassembly of the pinned executable finds 341 direct call instructions
to `FUN_140211e70`, spread across many caller regions. The resolver performs a
tree lookup, materializes a local record, calls the loader, then calls the
name-map slot helper to copy the record. The available Ghidra decompilation
does not show a lock around that sequence. The loader uses the manager's shared
stream for seek/read/decrypt, and no lock around the combined seek/read extent
has been identified. Standard iostream operations do not by themselves prove
that a multi-call seek-plus-read transaction is serialized.

The static evidence also does not establish whether higher-level callers can
serve a cached model without entering this resolver, or whether first-time
resource loads are confined to initialization. Thus cache insertion
serialization, stream serialization, nested-load interactions, and the real
installation window remain unresolved. The existing
loader's synchronization is distinct from additional wrapper behavior:

| Invariant | Evidence | Status |
|---|---|---|
| Each invocation edits only its own 0x58-byte descriptor | Resolver stack copy at `FUN_140211e70`; route helper writes/restores offset and size only | Verified statically and in native/Wine fixtures |
| Original loader ABI and call count are preserved | Exact call bytes and Windows x64 arguments; wrapper forwards context, name object and flags | Verified statically and in executable Wine fixture |
| Route publication is immutable and process-lived | Proxy static manifest/binding storage; release/acquire publication; relay retained to process exit | Verified in source and manifest fixture |
| Five-byte patch cannot overlap execution | Hook API requires external quiescence; startup fixture parks its sole caller before patch | Fixture-only; no equivalent game-wide exclusion established |
| Archive seek/read transaction is serialized | Shared stream observed in loader; no lock around seek/read identified | Unresolved pre-existing game property; wrapper adds no lock or I/O operation |
| Cache insertion and cache-hit behavior are safe for activation timing | Resolver copies the post-loader descriptor; outer cache lock and hit path not established | Unresolved; route must be enabled before first target request to affect a cold load |
| Nested loads do not deadlock | Wrapper adds no blocking or lock; nested behavior in game call graph not established | Wrapper introduces no new lock cycle; game recursion remains unknown |
| Replacement object is cached with original resolver metadata | Helper restores offset/size and preserves loader-written `+0x48` object pointer before caller cache copy | Verified helper contract; game cache semantics remain unverified |

The route is request-local: concurrent invocations use separate stack
records, and the wrapper adds no shared scratch descriptor, allocation, lock,
wait, or extra I/O. If the game's shared-stream seek/read sequence is racy, the
replacement read can be affected by that existing race, but changing the
requested extent does not create stream sharing. Restoring the original
metadata lets the normal cache-copy path retain the parsed-object pointer;
whether later cache hits depend on the restored extent is unknown. Cache hits
may bypass the callsite, so activation must precede the first relevant cold
lookup. No global loader mutex is added.

That operation is implemented as the allocation-free helper
`native/src/record_route.c` (`hyprlodr_record_route_invoke`). It requires an
already validated immutable route descriptor, checks exact logical name,
original offset/size, and tag `1`, and passes tag-2 `hand2` and unrelated
requests through unchanged. It validates the appended extent against archive
size and the loader's signed allocation limit. Native tests cover restoration
of the original metadata while retaining the parsed-object pointer,
same-name tag-2 rejection, stale metadata, invalid extents, reentrant calls,
and independent concurrent request descriptors. The same source is compiled
by the Windows cross-build. A small Windows x64 fixture linked against this
compiled helper also ran under Wine and verified the 22,056-byte replacement
extent plus rejection of the tag-2 `hand2` request. These are tests of the
request-level component, not of a game callsite detour.

The early environment opt-in installs a dormant hook during process-start
attach, before the executable entrypoint. Calls during bootstrap pass through
to the original loader exactly once; the worker enables routing only after
manifest validation and confirmation that the registered `asset/asset`
redirect matches the manifest archive. Route and callback state remain in
static process-lifetime storage, and disablement is atomic without unpatching.
The compiled Wine startup fixture exercises this lifecycle. It proves loader
ordering for its generated static-import client, not every initialization path
in the game.

The pinned executable directly imports decorated `std::_Fiopen` from
`MSVCP140.dll`; the relevant slot is in the executable's own IAT. The early
main-image patch therefore covers this known archive-open path before the
bootstrap worker begins. A compiled Wine fixture imports that exact decorated
symbol from a stand-in runtime DLL and verifies that archive requests wait for
redirect publication. It does not prove that every possible archive-open path
or every first cold `hand2` lookup in the real game uses this import. If a cold
`hand2` load completes while the route is dormant, the original object may be
cached and later requests can bypass the callsite. The ordinary game
stream/cache synchronization remains a separate pre-existing uncertainty; the
wrapper adds no shared descriptor, lock, wait, or additional I/O. Thus the
known first archive-open boundary is intercepted, while first-cold-model
ordering in the game remains unproven.

The append archive still cannot route itself: record offsets live in runtime
resolver tables, not a self-contained directory. Native preflight validates
hashes and range/resource identity; offline Python tooling separately verifies
decryption and parsed mesh counts. Remaining runtime questions are whether the
pinned game's dependency initialization can reach the callsite before the
proxy's attach completes, whether a first cold model lookup can precede route
activation, whether stream/cache behavior can affect the replacement extent,
and whether the enlarged object parses and reaches rendering. The
proxy remains opt-in and fails closed when any early install or archive
identity check fails.

`Group8673` remains an unknown 8-byte graph value (`00000000a4040000`); its
upper u32 numerically matches the original index count 1,188, but no consumer
was found that establishes a draw range or index-count role. It is preserved
unchanged in the edited payload. Consequently, increased mesh-array counts
are accepted by the offline parser, while graph-tail compatibility and actual
runtime draw count remain unverified.

The 72-byte record can carry the known position, normal, UV0, four-float,
tangent, and bitangent groups, but no vertex weights have been verified; the
four-float group is not identified as skinning weights. A custom feathered arm
can be encoded as geometry only if its attribute requirements map to these
fields. Keeping native posing/animation is a separate compatibility question:
the model's relationship to `handbones`, per-vertex rig data, procedural pose
updates, material grouping, and runtime normal/tangent recomputation remain
unresolved. Native-format serializability alone does not establish animation or
rendering compatibility.

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

Create the attribute-preserving Blender mesh, edit its points/faces, and export
an edit document. For every added Blender point, set the integer point attribute
`hyprlodr_source_vertex` to a valid source vertex index; this selects the template
for the new native record's opaque bytes. The exporter writes indexed triangles
and splits vertices where loop UVs require a seam:

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
`handveins` still rebuild byte-for-byte. For `hand2`, source and rebuild
SHA-256 were both
`ddfe1797efd29c1200e1c775d998b73ef73b9b0e8710e92d7b632c3a29c93fa8`.
Topology fixtures using each of `hand2`, `hand2left`, and `handbones` produced
three valid variants: an attached triangle (+1 vertex, +3 indices), one removed
face (-3 indices), and reversed vertex order with remapped indices. All retained
the 72-byte stride and exact graph-tail bytes. The Blender 5.2.2 pipeline also
round-tripped an edited `hand2` with an added edge-connected triangle to 239
vertices and 1,191 indices. Its 22,056-byte output encrypted with the installed
executable's real cipher tables and `hand2` seed, then decrypted byte-identically.
Generated payloads, Blender files, ciphertext, edit JSON, and logs are under
ignored `.tmp/`.

### Source-arm identity assessment

The capture evidence in `RE_HAND.md` establishes that the deliberately modified
tag-1 `hand2` geometry reached the recognized hand draw: the captured indices
match both variants and its entire vertex buffer matches the 1.35× edited payload
byte-for-byte. This topology-editing milestone did not perform another game run.
The new serializer only establishes binary-format compatibility; it does not
establish that an arbitrary changed topology remains compatible with the game's
opaque graph property or animation behavior.

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

## First-load event capture

`HYPRLODR_FIRST_LOAD_DIAGNOSTICS=1` enables a passive, bounded event recorder.
The early proxy attach and intercepted loader callbacks only copy fixed-size
records into a 1,024-entry static ring. The bootstrap worker creates per-run
startup and file JSONL logs, drains the ring, and starts a dedicated writer thread;
each JSONL event has a shared atomic `seq`, PID, run ID, monotonic process tick,
thread ID, event kind, archive gate state, and
the relevant resource tag/extent/route flags. The first archive open record
includes the actual path selected. First loader-entry and return events retain
the original descriptor values and parsed-object pointer. A drop marker means
the trace is incomplete. This mode does not activate routing.

The intended diagnostic run uses the original archive and
`experimental_record_route: false`, so any `hand2` request passes through the
original loader unchanged. The trace answers whether the first cold target
request follows archive and route readiness; an absent target event is only
meaningful if the first general callsite event and complete, non-overflowed
trace establish that the instrumentation ran.

The attach record reports requested/attempted/installed hook states, writer
startup status, and total ring drops. The proxy keeps its writer for process
lifetime; it does not wait for it in `DllMain`. A safe explicit finish API is
used by the compiled fixture to verify final draining. The offline report
generator correlates per-run startup and `_Fiopen` logs and marks missing
instrumentation as unknown unless the process exited with a complete,
non-overflowed trace and an observed intercepted callsite.

### Controlled diagnostic-only run (2026-11-05)

The executable fingerprint matched all 9 anchors. With route configuration
disabled and the installed archive unchanged, the file log recorded the first
`asset/asset` open through the executable's `_Fiopen` import at relative
`t_ms: 275`; the original relative path opened successfully and was not
redirected. There was no `redirect_wait` entry, consistent with the archive
gate already being released by the time of the open. That ordering is an
inference because the startup ring stopped after its two attach-time entries.

The trace is incomplete: it contains no bootstrap, gate-release, resolver
callsite, `hand2`, or loader-return event. The early-hook event says
`installed_dormant`, while the final attach record says the early hook was not
requested; this contradictory pair is not accepted as proof of live hook
installation. The OpenGL log shows the game reached its rendering loop, but no
gameplay navigation, first-person hand draw, or model parse was established.
No variable-size replacement was attempted.

The missing ring drain was corrected by starting a dedicated writer thread
from the bootstrap worker and directly draining bootstrap milestones. A
compiled Wine fixture now checks shared event sequencing across two producer
threads. The real-game result was not repeated: the initial attempt launched
through the already-running Steam client, which placed the game on shared
display `:0` instead of the isolated `:99`; startup also updated files under
Proton's HyperDemon AppData directory. The proxy, mods, root configuration,
runtime logs, and prior GL captures were restored, and executable/archive
hashes remained unchanged. The pre-run AppData contents were not snapshotted,
so their exact original contents could not be restored. This is a remaining
runtime-testing limitation.

### Follow-up trace and isolation audit (2026-11-05)

The previous attach, startup, and file artifacts had no common run ID; their
local sequence counters also collided. Startup reports `installed_dormant`,
while the final attach JSON says the early hook was not requested and has
`not_requested` status. The saved files cannot be proved to belong to one
proxy instance, so the exact historical cause is unresolved. Unique per-run
filenames and IDs now make this ambiguity detectable. The machine-readable
review at `.tmp/first-load-diagnostic/evidence/first-attempt/coverage-review.json`
confirms the original `asset/asset` open in the file log, but correctly marks
hook execution and tag-1 `hand2` ordering unknown.

No second launch was made. The earlier preflight treated any active Steam
process as a veto, while the actual requirement is preventing IPC, Cloud, and
filesystem access. A new Bubblewrap probe can establish a private PID/network
namespace, no default route, inaccessible host abstract sockets, read-only
host filesystems, and disposable HOME/`/tmp`/`/run` mounts while Steam remains
active. The prior attempt's Xvfb display collision is also avoided by creating
Xvfb inside the same private mount namespace with `-displayfd`.

`tools/re/isolated_proton.py` stages a new copy of the verified game files,
omits installed mods, user state, crash artifacts and captures, installs the
current proxy plus the hash-verified shipped OpenAL runtime into that copy, and
uses a fresh compatdata prefix. A run is claimed before preflight and cannot be
launched a second time; a new run ID is required after success or failure. The
original game directory and archive are never deployment targets. The game
process has no Steam client IPC or network route; it receives the real app ID
read from the installed game's manifest but no Steam credentials or ownership
spoof. If HYPER DEMON requires an authenticated client for initialization, the
isolated diagnostic can stop there. Cloud sync cannot occur in this mode. Each
run has a preflight JSON, launch environment, pre/post file inventories, and a
process-scoped event report. The Proton child uses the disposable `WINEPREFIX`
and is terminated as an owned process group on timeout. A startup trace does
not claim gameplay coverage; absent `hand2` requests are limited to the
observed process lifetime and do not prove the model would not load later.
No variable-size replacement is enabled by this runner.

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

## First persistent passive GPU hand capture

The 2026-11-04 run produced a valid raw `HYPRCAP1` v1 capture of the 238-vertex,
1,188-index draw signature with 72-byte vertices. Detailed header validation,
payload hashes, attribute checks, source comparison and restoration evidence are
recorded in `docs/RE_HAND.md`; the raw capture and comparison report remain in
ignored `.tmp/` storage.

The captured index array exactly equals tag-1 `hand2`'s original and edited index
arrays. The vertex buffer exactly equals the 35%-enlarged edited payload byte for
byte, including every supported and uninterpreted attribute. The original payload
has exact index and non-position attribute correspondence; after the analyzer's
unique remap, its positions map to the capture with a diagonal 1.35 affine scale,
translation `(0.00254956, 0.01428750, -0.00005941)`, RMSE `4.09e-9` and maximum
residual `1.28e-8`. This is strong evidence that edited `hand2` geometry reached
the instrumented hand draw. Since the snapshot records the final OpenGL vertex
buffer, rather than a loader return buffer, a precisely identical CPU position
rewrite of original geometry remains a theoretical alternative. A future test
should apply a distinctive non-affine per-vertex displacement to distinguish
source replacement from a global runtime transform.

The input helper confirmed the foreground HYPER DEMON window and sent the
historical Win32 PLAY sequence. The saved frame 480 is from before that action
and shows the title menu; the later screenshot is black. No runtime evidence
confirms transition to gameplay, so the capture cannot be certified as having
occurred during gameplay or as proof of a playable first-person scene. A
minidump created about two seconds after the input/capture records an access
violation at `hyperdemon.exe+0x7c67`, reading `0x90` through a null vtable in
Ghidra function `FUN_140007640`. It identifies the immediate dispatch fault but
not its subsystem or cause; no causal link to the input, model or passive hook
is established.
