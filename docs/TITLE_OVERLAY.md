# Title overlay

**A run with at least one mod shows HYPRLODR title art over the game's title screen. A run with zero mods shows the game's own title screen, untouched.**

## Why it is an overlay and not a replaced asset

The title screen's own art lives inside `asset/asset`, and that container is not decoded yet: the index is read through the loader's stream cipher, and the surviving problem is the framing/compression, not the cipher (see `docs/RESEARCH.md`). Replacing the *rendered* title screen is therefore blocked on the container format.

What is not blocked is the layer above it. The proxy (already inside the game process) owns a topmost, click-through, per-pixel-alpha window sized and positioned over the game window. The player sees HYPRLODR art instead of the game's title screen; the game's real menu is still underneath and still accepts clicks.

This is honest about its limits: it is a window above the game, not a change to what the game draws. Nothing in the game's memory or files is modified.

## Behaviour

| Situation | Result |
|---|---|
| Loader installed, **≥1 mod initialized** | overlay shows `hyprlodr-title.bmp` over the game window |
| Loader installed, **0 mods** | nothing is created; the game's own title screen |
| Loader not installed | no overlay code exists in the process |

- The window uses `WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED`, so it never takes focus and never swallows a click: the menu underneath stays usable.
- It hides on the first key or mouse press (after a 900 ms grace so the launch click cannot dismiss it), or after `title_ms` from `hyprlodr-config.json` (`0`, the default, means "until the player presses something", capped at 10 minutes).
- Everything is logged: `hyprlodr-title.jsonl` next to the game records `shown` with the window size, then `hidden`. The attach record carries `title_overlay` (`starting`, `shown`, `image_invalid`, `no_window`, ...).

## Replacing the art

The overlay reads an uncompressed BMP because the Windows target has no image decoder and shipping one into the game process is not worth it. Convert your artwork once:

```bash
python3 tools/hyprl_title.py convert my-title.png "<game>/hyprlodr-title.bmp" --width 1920
```

Any aspect ratio works: the overlay scales the image to the game window with nearest-neighbour sampling, and bright pixels become opaque while near-black pixels become transparent, so the art's black background does not hide the game.

A placeholder is included at `assets/title.bmp` and is copied into the game directory by the installer only when no title image exists yet, so installing never overwrites art the user put there. `tools/hyprl_title.py placeholder out.bmp` draws one procedurally.

## Verified

2026-11-01, real game through Steam/Proton, `delay_ms: 0`:

- one mod (`example.game-info`): `hyprlodr-title.jsonl` → `{"status":"shown","width":1288,"height":754}`, 6/6 mods initialized, game alive, no crash record; the screenshot in `docs/evidence/2026-11-01-title-overlay.png` shows the wordmark over the game's title screen;
- zero mods: `title_overlay: not_started`, no title log, no overlay window, `initialize_ok` count 0 — the game's own title screen is what the player gets.

## Limitations

- It is an overlay, not the game's own render pass: it does not survive a mode change that recreates the game window (fullscreen toggle, resolution change) — the window is sized once when it is shown.
- Dismissal by input could not be confirmed under Wine with synthetic `xdotool` keys; `title_ms` is the reliable path there, and a real key press is what the polling loop watches for.
- The art is a single 24/32-bit BMP; no alpha channel is required (brightness is used), and animated or video art is out of scope.
- The manager's own installer does not yet copy `assets/title.bmp`; the CLI installer does. Until it does, the manager route needs the image copied by hand.
