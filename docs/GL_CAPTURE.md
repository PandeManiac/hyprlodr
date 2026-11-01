# In-process OpenGL capture

A research instrument that runs inside HYPER DEMON (through the same `OpenAL32.dll`
proxy the loader uses) and records what the renderer actually does. It exists
because the title screen has to be replaced *inside* the game's own rendering, and
that requires knowing its draw calls, programs, textures and atlas — not guessing
them.

It is not part of the mod API and it does not change what the game draws.

## How it works

HYPER DEMON imports no GL entry point at all. GLFW loads `opengl32.dll` at run time,
and under Proton that resolves to Wine's `opengl32.dll` → Mesa GLX. Two resolution
paths exist, and both are patched with shims by symbol (no static addresses, no
game-code patching):

| Path | Where | Why |
|---|---|---|
| `KERNEL32!GetProcAddress` | every loaded module | GLFW's WGL extension lookups and any engine-side loader |
| `opengl32!wglGetProcAddress` | every module that imports it | every GL 2.0+ entry point |

`wglSwapBuffers` is intercepted through `GetProcAddress`, which gives a callback
once per frame with a live context. From there the capture uses GL 1.1 entry points
that `opengl32` exports directly (`glGetString`, `glGetIntegerv`, `glReadPixels`) to
record the GL version, renderer, viewport, current program, bound texture, and one
BMP frame.

Output: `hyprlodr-gl.jsonl` (and `hyprlodr-frame.bmp`) in the game directory. Shader
text, if captured, is game content: the log stays local and is never committed.

## Verified in the real game (2026-11-01)

- Interception installs: `gl_capture: installed`, `GetProcAddress+wglGetProcAddress
  slots=29 modules=49` in the attach record.
- The swap shim is called with a live context: `GL 4.6 (Compatibility Profile) Mesa
  26.2.5`, renderer `AMD Radeon RX 9070 XT (radeonsi, gfx1201)`.
- The framebuffer readback is real: a 1920×1080 frame was written and verified by
  decoding it (the game's startup splash, mid-load). The image is game content, so it
  stays local; the evidence records its sha256 instead of the pixels.
- The last-draw state is visible at swap time: at frame 120 the bound 2D texture was
  `111`; the current program queried as `-1`.

## The finding that matters

The engine does **not** resolve its GL entry points through either patched path.
Only four WGL names were ever requested (`wglChoosePixelFormat`, `wglSetPixelFormat`,
`wglDescribePixelFormat`, `wglSwapBuffers`), and no `gl*` name was requested at all —
no `glCreateShader`, no `glShaderSource`, no `glDrawArrays`. There are no delay-load
imports (`delay_imports rva=0`), and re-patching every 250 ms changes nothing, so this
is not a clobbered IAT slot.

**Observed:** the renderer reaches OpenGL by some route other than the two symbol
resolution paths, and executes its draws with no program bound at swap time
(`program=-1` on a 4.6 compatibility context).

**Inferred (not yet proven):** a compatibility-profile fixed-function path or an
internal GL dispatch table installed before the proxy's DllMain. Distinguishing these
is the next experiment: intercept `wglGetProcAddress` at its `opengl32` export (a
detour inside the Wine DLL) or hook `glXGetProcAddress`/the GLX layer below it.

## Consequences for title replacement

Until the title's draws are observable, a replacement cannot reuse the original
shader or atlas, and drawing something on top is explicitly not the goal. The
capture instrument is therefore the prerequisite, not the deliverable: it already
proves in-process framebuffer and GL-state access works under Proton, which is
what any texture-level glyph swap will need.
