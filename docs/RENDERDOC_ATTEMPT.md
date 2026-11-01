# RenderDoc against HYPER DEMON (2026-11-01)

Goal: capture a real frame of the game's OpenGL title screen with an existing
graphics debugger instead of extending our own instrumentation. Result: **no capture
was obtained, for three independent reasons**, all recorded here.

Local tooling: `renderdoc` 1.46-1 (Arch), `renderdoccmd` v1.46, APIs compiled in:
Vulkan, GL, GLES; windowing: xlib, XCB. No `apitrace` is installed.

## Failure 1 — the Wine launch path never starts the game

`renderdoccmd capture` / `qrenderdoc` launch a *Linux* executable. HYPER DEMON is a
Windows PE running through Proton, so the documented approach is to preload
`librenderdoc.so` into the Wine process, i.e. into the process Steam spawns:

```bash
LD_PRELOAD=/usr/lib/librenderdoc.so RENDERDOC_TEMP=/tmp/rdcap \
  "$STEAMROOT/steamapps/common/Proton - Experimental/proton" run ./hyperdemon.exe
```

With `DISPLAY=:0`, `STEAM_COMPAT_CLIENT_INSTALL_PATH` and
`STEAM_COMPAT_DATA_PATH` set, the Proton wrapper aborts before the game's first
frame:

```text
Unhandled exception in Xalia:
System.ApplicationException: No displays available
  at Xalia.Sdl.SdlSynchronizationContext.Init (SDL3.SDL+SDL_InitFlags)
[ERROR] FATAL UNHANDLED EXCEPTION
```

`hyperdemon.exe` never appears (`pgrep -x hyperdemon.exe` → empty), so no GL context
is ever created and RenderDoc has nothing to hook. Note that the same game directory
launched through Steam works normally, so this is the wrapper's Steam/Xalia
integration, not the game.

## Failure 2 — injection produces no capture

`renderdoccmd inject` was attempted against a Steam-launched process. The invocation
printed its usage text and returned without writing a `.rdc` (`/tmp/rdinj` holds only
RenderDoc's own directory; no capture file anywhere under `/tmp` matched
`*.rdc`). Injecting into a live GL process after context creation is also the wrong
time for GL: RenderDoc's GL capture hooks context creation, not existing contexts.

## Failure 3 — the context is a compatibility profile

Even with a working hook, RenderDoc's OpenGL replay expects a core profile
(the FAQ: "For OpenGL RenderDoc will only capture core profile applications, in
general, and expects at minimum to be able to create a core 3.2 context").

**Observed in the game** (our own in-process query, `docs/evidence/2026-11-01-gl-capture.json`):
`GL_VERSION = 4.6 (Compatibility Profile) Mesa 26.2.5`. The title therefore runs on a
context RenderDoc does not support for capture/replay.

## Side effect, and why it is not this repository's code

After these attempts the game aborts at start-up **without the loader installed**:

- `OpenAL32.dll` sha256 `396f88738ce20905e933cb7d8c696351e41bd7e72e80b8cfdadf20ab613420b4`
  (the shipped file), and no `hyprlodr-install-manifest.json`;
- `hyperdemon.exe` sha256 `7612dd72…3611` and `asset/asset` (306,794,369 bytes) unchanged;
- `steamuser.log` ends with `_crash_`, and `crash_steamuser.dmp` is written;
- the process is gone within 3 s of launch and no window appears.

Because the unmodified game reproduces it, the cause is the Steam/Proton environment
(most plausibly the compat prefix having been used concurrently by the direct
`proton run` attempts above, which is the documented hazard of running the same
`STEAM_COMPAT_DATA_PATH` while Steam holds it). Recovery is a Steam client restart
and/or Steam's "Verify integrity of game files"; neither was performed here because
both touch the user's session rather than this repository.

**Consequence:** the RenderDoc route is closed for this build (compatibility profile),
and in-game verification is unavailable until the environment is recovered. The
in-process capture committed earlier (`native/win32/gl_hook.c`) remains the working
instrument: it already reads GL state and a full 1920×1080 frame from inside the game.
