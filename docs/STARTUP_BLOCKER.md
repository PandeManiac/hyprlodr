# Startup blocker and what the binary says about the title (2026-11-01)

Two separate results from this session: a hard blocker on running the game, and
static evidence about how the title is built.

## 1. The game now exits seconds after launch (environment, not mod code)

Reproduced consistently through `steam -applaunch 1743850`, with **the loader
uninstalled** (`OpenAL32.dll` = shipped `396f8873…420b4`, no install manifest) and
with the game's own leftover files moved aside. `content_log.txt`:

```
AppID 1743850 state changed : Fully Installed,App Running,
AppID 1743850 state changed : Fully Installed,      (2 s later)
```

### What is ruled out

| Suspect | Measurement | Verdict |
|---|---|---|
| GPU/GL broken | `glxinfo -B`: direct rendering, RX 9070 XT, GL 4.6 core+compat | ruled out |
| Steam client degraded | restarted; 18 steam processes, 12 `steamwebhelper` reached, still fails | ruled out |
| Corrupt game config | backed up, removed; game never regenerated it | ruled out |
| HYPRLODR leftovers | all `hyprlodr-*` moved out of the game dir; still fails | ruled out |
| Corrupt prefix registry | `system.reg` 4,698,021 B, `user.reg` 315,829 B, both freshly written, no overrides for this title | not supported |

### What the crash dump actually says

`crash_steamuser.dmp` (59,737 B, Mini DuMP, 8 streams) parsed with a small
minidump reader:

- exception `0xC0000005` (access violation), faulting address **`0x90`**
  (`ExceptionInformation = [0x0, 0x90]` → read of 0x90, i.e. null + 0x90),
  thread 328;
- **63 modules** loaded, including `steam_api64.dll` and `hyperdemon.exe` at
  `0x140000000`, so the process got well past startup;
- faulting frame inside `C:\windows\system32
tdll.dll` (+0x714e0).

That dump is from 04:53. **Later launches write no new dump and do not even recreate
the config file**, so the current exit happens *earlier* than that crash — the game
is being terminated during early start-up, before the crash handler is armed.

### Recovery, in order of least damage

1. Steam → HYPER DEMON → Properties → Installed Files → **Verify integrity**.
2. If that fails, the prefix is the remaining suspect. Its state lives in
   `compatdata/1743850/pfx`; the game's saves and settings are in
   `pfx/drive_c/users/steamuser/AppData/Roaming/HyperDemon` (`config`, `user`,
   `cache/scores`, `cache/names`). Back that directory up, then let Steam rebuild
   the prefix (delete `pfx` with Steam closed). **Back up first** — scores and names
   live there.

A copy of the current `config`/`user` is already kept in `/tmp/hd-backup`, and the
moved `hyprlodr-*` files are in `/tmp/hd-leftovers`; `/tmp` is not durable, so rely
on the backup above only for this session.

## 1b. ROOT CAUSE FOUND: Steam is not signed in

Steam's own console log ends in logon failures, not a game fault:

```text
LogonFailure Service Unavailable
LogonFailure No Connection
LogonFailure Service Unavailable
Logged On, 4, 7] [U:1:193590130] LogOff()
```

With the client logged off, `SteamAPI_Init` fails in `steam_api64.dll` and the game
exits deliberately: no window, **no new crash dump**, and the per-user `config` is
never recreated (both confirmed). That also explains why the 04:53 minidump is the
*last* one — that crash predates the logoff.

So the blocker is an account/login state, not the game, the prefix, HYPRLODR, or the
GPU. The fix is to sign in to Steam (Steam → Sign in), after which the game should
launch exactly as before; the earlier prefix-rebuild advice in this document is
superseded and should not be needed.

## 1c. RESOLVED: the launch failure was a dead X screen, and a virtual one fixes it

The 1b diagnosis (signed out of Steam) was correct but incomplete: signing in was not
possible either, because **Steam's client UI could not create a display**:

```text
[2026-11-01 06:33:00] Couldn't initialize SDL: The video driver did not add any displays
```

The KWin Wayland session's Xwayland virtual screen had collapsed to zero size:

```text
$ xdpyinfo | grep dimensions
  dimensions:    0x0 pixels (0x0 millimeters)
$ xdpyinfo | grep resolution
  resolution:    -2147483648x-2147483648 dots per inch
```

Steam is an X11 client, so with no screen it came up without a window, never attempted
a logon (`connection_log.txt` shows no `LogOn()` after the 04:53 `LogOff`), and
`-applaunch 1743850` was silently a no-op. No desktop capture worked either (`xwd` and
`grim` both refuse that 0x0 root), so this could not be fixed by clicking anything.

**Working recipe (verified this session, no physical display needed):**

```bash
setsid Xvfb :99 -screen 0 1920x1080x24 +extension GLX +extension RANDR +render -noreset &
DISPLAY=:99 glxinfo -B     # llvmpipe, GL 4.6 Compatibility -> acceptable to the game
setsid env DISPLAY=:99 steam -cef-disable-gpu &          # auto-login, ~12 s
setsid env DISPLAY=:99 steam -applaunch 1743850 &
```

Observed results: `connection_log.txt` reached `[Logged On, 4, 7] [U:1:193590130]`, the
client authenticated, Proton started the prefix, and the game ran with a 1920x1080
`hyperdemon` window; the game's own `steamuser.log` reports
`Steam running..[berds]..login( )`. **llvmpipe is the renderer**, so timing is slow but
frames are correct.

**What the loader reports in that run** (`hyprlodr-attach.json`): `loader_abi` 7,
`build_matches` true, `anchors_matched` 8/8, `tables_dumped` true, `file_hook`
`installed` (`CreateFileA`, `fopen`, `fopen_s`, `_wfopen`, `_Fiopen` patched across 39
modules), `gl_capture` `installed` (`GetProcAddress`+`wglGetProcAddress`, 27 slots, 46
modules, growing to 42 slots / 72 modules in `hyprlodr-gl.jsonl`). A 1920x1080 frame was
dumped behind `wglSwapBuffers` at frame 120.

**Baseline of the original title (three captures, `import -window`, not committed):** the
lettering is enormous **black glyph silhouettes** — `HYPER` across the upper half,
`DEMON` across the lower — cut into vertical strips that repeat horizontally, with the
animated iridescent field visible in the gaps and inside the counters, and the menu row
(`PLAY OPTIONS TRAINING CLIPS LEADERBOARDS EXIT`) on a black band between the words. The
mean luminance of successive captures changed (10669 -> 11855 over ~4 s), so the effect
is animated. This is consistent with the title being a **mask/stencil over a full-screen
shader effect** rather than a lit 3D wordmark, and it means the replacement has to supply
glyph *shapes* in the same mask, not coloured art of its own.

## 2. What the binary says about the title (static, verified)

**Observed** (`strings` on `hyperdemon.exe`, sha256 `7612dd72…3611`):

- the menu labels are plain literals: `PLAY`, `OPTIONS`, `TRAINING`, `CLIPS`,
  `LEADERBOARDS`, `EXIT`;
- **neither `HYPER` nor `DEMON` appears as a literal**, in ASCII or UTF-16;
- asset-name strings in the binary include `entity_lobby_dagger_hyper`,
  `entity_lobby_dagger_hyper2`, and a `HyperDemonInstanceLock` mutex name;
- exactly one shader name string exists: `skull_giga.vert` (twice);
- font-ish strings are present (`glyph-dict`, `font-format`, `FontName`,
  `FontMatrix`, `FontBBox`, `FontDirectory`, PostScript/CID font headers), but
  there is **no** `FreeType`/`HarfBuzz`/`stb_truetype`/MSDF string.

**Inferred (hypothesis, not proven):** the menu's text labels go through a glyph
pipeline (the labels exist as strings and the binary carries font-dictionary
vocabulary), while the enormous title is **not** a text literal — it is more likely
asset-driven: either a wordmark texture, or lobby entities/meshes named
`entity_lobby_dagger_hyper*` rendered with the iridescent material. That distinction
decides the whole replacement strategy, and it cannot be settled statically because
the container that holds both possibilities is still undecoded.

**Next experiment (needs a running game):** the texture census behind the working
`wglSwapBuffers` hook — enumerate 2D textures at the menu frame, dump the largest
few, and look for a glyph sheet. If found, the letters are atlas quads and the
replacement is a cell swap that keeps the original material and animation; if not,
the letters are geometry and the target becomes the `entity_lobby_*` meshes.
