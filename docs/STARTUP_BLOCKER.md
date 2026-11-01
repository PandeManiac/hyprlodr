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
