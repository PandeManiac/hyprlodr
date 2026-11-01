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
