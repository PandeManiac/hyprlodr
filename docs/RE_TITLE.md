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

Why this matters: the lettering is addressed *by name inside the pack*, so a replacement can
be a same-tag payload served under the same name — which is exactly what the `_Fiopen`
redirect layer is for. What remains unknown is the record's tag and its payload layout, and the
pack (`asset/asset`, 306,794,369 B) is still undecoded, so neither can be read offline.
