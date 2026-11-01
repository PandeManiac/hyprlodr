# Session progress

## 2026-11-01 (twelfth pass: persistent hand-draw hook, live suppression)

**The hand draw now has a production hook, not just a GDB breakpoint.** `examples/hand_hook.c`
(`example.hand-hook`) gates on `build_matches()`, reads the 8-byte slot at RVA `0x3d9ac8`
(live value `0x7bee1450`), writes its shim through ABI 6 `image_write`, verifies the readback,
and restores the slot with `image_restore` on shutdown. The shim forwards every unrelated
draw and matches the hand by `GL_TRIANGLES`, `count == 1188` and the index type. Mode is read
from `<state_dir>/hand-mode.txt` (`suppress` or pass-through).

**Suppression is visually verified in the real game.** The mod logged `patched 8 byte(s) at
rva 3d9ac8` and `dispatch hook installed ... (original=0x7bee1450 mode=suppress)`, initialized,
and gameplay rendered the world and birds with **no first-person arm** (1920x1080, sha256
`cbc0acecd0fb0e516e989bd08a9e719f17941ddbef40135343f7ada2b901a71d`, not committed because it
contains game content). The process was alive at capture.

**The index type was wrong all along.** The live draw declares 5125 = `GL_UNSIGNED_INT`, and
the 4,752-byte element buffer is exactly 1,188 `uint32` indices (4,752 = 1,188 x 4), not two
16-bit runs. The old cube IBO was therefore 72 bytes read as 32-bit indices, which explains
the crash after the first substitution. The mod and `docs/RE_HAND.md` are corrected.

**Two honest limitations.** The first shim logged through the host API from the render thread
and the game died on the first gameplay hand draw; the shim is now pure (count, suppress,
forward), and the host is only called at initialize/shutdown. The current build also exited a
few seconds after the verified capture with no crash record and no shutdown log, so sustained
suppression stability is unproven. The next test runs `pass` mode for the same duration to
separate suppression from the hook's presence, then substitutes geometry for a solid test
mesh through the same slot.

## 2026-11-01 (eleventh pass: live hand-draw recheck)

**A second live Steam/Proton process confirmed the hand signature after the previous game had
exited.** At `count=1188`, mode `GL_TRIANGLES`, type `GL_UNSIGNED_SHORT`: attribute buffer
41, element buffer 42, and `indices` pointer **NULL**. The six attribute offsets are
`0, 12, 24, 32, 48, 60`, matching the helper's layout.

**A larger cube still is not a usable replacement.** A 0.5 half-size cube at the
bounding-box centre still rendered only as illuminated fragments; mixed per-corner source
records were prepared to vary UVs and skin data, but the 200-draw trace auto-restored before
a verified mid-injection capture. The original arm is back on screen. This rules out an
index offset and outward winding as the explanation; inherited UV/bone/material state or
placement remains.

## 2026-11-01 (tenth pass: validated 16-bone runtime arm)

**The authored arm now fits the game's 16-matrix skinning budget without discarding its
feather rig.** `tools/assets/bake_runtime16.py` creates a second armature and mesh in the
editable project: five core bones, two merged digit groups and all nine feather rows. The
original 26-bone authoring rig, weights and actions remain untouched in the same `.blend`.

**Validation is numeric, not just a successful script run.** The bake reports 16 runtime
bones, max 3 influences per vertex, a 3.0e-8 weight-sum error, a 1.8e-7 rest-matrix error,
and a 2.7e-7 baked pose-matrix error across all seven actions. The runtime GLB contains
exactly seven `runtime_*` animations and one 16-joint skin. The runtime JSON also records
all 4,716 normalized vertex weights and per-frame bone channels.

**The runtime was inspected visually.** `runtime_fp_idle`, `runtime_fp_dash`,
`runtime_fp_bunnyhop` and `runtime_fp_stomp` use the first-person camera and show the blue
faceted arm, layered feather rows and claws surviving the bake. The four finger chains are
rigid in the runtime; independent claw curl remains an authoring-only detail. This is still
not a game replacement: the material/draw integration is the next step.

**Hand attribute offsets were also captured:** `0, 12, 24, 32, 48, 60` bytes. The test cube
used the correct position/normal offsets, so the remaining cube fragment is a placement and
inherited-weight problem, not an offset guess.

## 2026-11-01 (ninth pass: hand draw located, live buffer substitution proved)

**The first-person hand is no longer invisible to the capture.** The engine's
`glDrawElements` does not go through an import slot: at `hyperdemon.exe+0x2370c0` it calls
through its own dispatch table at RVA `0x3d9ac8`, which lands on the PE `opengl32` thunk.
Breaking there in host GDB showed 126 draws per frame. Two adjacent draws with
`count=1188` are the hand: zeroing only that count removed the arm while the world kept
running, and restoring it brought the arm back.

**The live resources are now measured.** At the hand draw: 238 vertices, 72-byte stride,
17,136-byte attribute buffer 41, 4,752-byte element buffer 42, indices
`GL_UNSIGNED_SHORT`, six enabled float attributes (3, 3, 2, 4, 3, 3). The first three
floats are position and the next three are normal; the rest carries tangent/UV and the
skinned-entity fields. The index buffer holds two 1,188-index runs.

**Buffer substitution is real, but the test mesh is not valid yet.** Replacing the first
eight vertex records and first 36 indices with a cube and drawing only those indices
removed the arm and produced a small fragment. Replacing the records while leaving the
original count visibly stretched the arm into large triangles. That proves the live draw
consumes buffers 41/42; the cube inherited one vertex's weights and was not placed at a
usable hand pivot. A second run with every triangle wound outward produced the same
fragment, so back-face culling is not the cause. The original VBO/IBO were then written
back and a live screenshot confirmed the original arm returned.

**New committed artifacts:** `docs/RE_HAND.md` now records the draw signature, buffer
layout and precise limitations; `docs/evidence/2026-11-01-hand-draw.json` holds the
metadata-only evidence; `tools/re/hand_vbo_test.py` reproduces a same-format test cube
from a local dump without embedding game bytes. The persistent dispatch-table hook,
attribute offsets/bone palette capture and the 16-bone bake are the next blockers.

## 2026-11-01 (eighth pass: arm art target, rig validation, spring runtime)

**The replacement arm now exists as a reproducible authored asset, not a game-side replacement.**
`tools/assets/build_berdly_arm.py` generates `assets/berdly-arm/berdly_arm.blend`, `.glb` and
`.mesh.json` plus twelve inspected renders. The latest pass anchors every feather to a sampled
forearm surface point, adds a tapered quill and raised rachis, rebuilds the hand as a curled claw
with knuckle masses, and moves the palette to deep blue with blue/cyan/violet iridescence instead
of the earlier pink/gold drift.

**Measured from the generated runtime description:** 4,716 vertices / 4,570 faces, 26 authoring
bones, 9 feather rows, max 3 influences per vertex, normalized weights, UVs on every face, and
seven baked actions (idle, dash, dash_slide, jump, stomp, bunnyhop, fake). The arm is deliberately
faceted and uses the authored Blender materials as an art target; the eventual runtime is meant to
reuse `entity_hand`/`entity_hand_vs`, not these materials.

**Added `native/src/feather_spring.c` + public header + CTest.** Fixed 240 Hz substeps, target and
angle limits, overshoot clamp with velocity reversal, reset for teleports, and invalid-input
handling. `feather_spring_test` passes; it checks 30/60/144/240 Hz convergence, bounded response,
reversal and reset.

**Honest blocker:** the authoring armature has 26 bones, but the verified `entity` path supplies at
most 16 `bone_transforms`. The mesh metadata records `runtime_skinning_status:
pending-16-bone-bake`. Milestone B is still blocked on observing the hand draw path; the renders are
art inspection only, not game-render verification.

## 2026-11-01 (seventh pass: the old overlay is gone)

**Removed the copper HYPRLODR wordmark overlay**, which was the pre-RE placeholder: a topmost
click-through GDI window painting `hyprlodr-title.bmp` over the game. With the mask replacement working
it was pure clutter, so the whole feature is gone - `native/win32/title_overlay.c`/`.h`, its build and
link lines, the proxy's include, `title_ms` parser, attach-record field and start call, the installer's
`assets/title.bmp` delivery, `tools/hyprl_title.py`, `docs/TITLE_OVERLAY.md` and the art itself.

**Verified in game after removal:** the run still logs `title_mask_loaded` and `title_mask_found` once
each, no `hyprlodr-title.jsonl` is written, the attach record no longer carries `title_overlay`, and
frames 170/184 show bare HYPR over LODR in the game's own animated fill with no second wordmark.

**Tests:** package payload 5/5, mask installer 3/3, installer safety 3/3, package 19/19.


## 2026-11-01 (sixth pass: the replacement ships as a mod package)

**`example.title-replacement` now exists.** The package format gained a `files` list: payload under
`assets/` can be installed beside the game (`to` relative to the game directory), hashed into the mods
index, and removed only while the bytes still match. That is what the replacement needed - the mask is
data the game's own title pass uploads, not code the loader calls.

**New files:** `examples/title_replacement.c` (reports whether the mask is in place, and publishes
`example.title-replacement`), `examples/package-title/manifest.json` (ships `assets/title-mask.raw` ->
`hyprlodr-title-mask.raw`), a staging step in `tools/build_windows.sh`, and
`tools/tests/test_package_files.py` (5/5).

**Runtime addition:** the minimal Windows CRT had `fopen/fgets/fprintf` but no `fread`, so the mod
could not count the mask's bytes; `fread` is now implemented over the existing buffered handle
declared in `native/win32/crt/stdio.h`.

**Verified in game:** loader installed deliberately *without* a mask, package installed, game started -
`title_mask_loaded` once, the mod logged "title mask in place (853x480 RGBA); the game draws HYPR over
LODR", and the frames show HYPR over LODR. Removing the package removed the mask; the installation went
back to pristine.

**Tests:** package 19/19, package payload 5/5, mask installer 3/3, installer safety 3/3.


## 2026-11-01 (fifth pass: the face comes out of the installation)

**The typeface no longer has to be extracted by hand.** `tools/title_mask_from_game.py` reads the
proxy's record log, cracks each slice (1024 keys), strips the 24-byte slice header every font carries,
identifies faces by their `name` table and renders the mask. On the live installation it recovers all
eleven shipped faces and produces a mask **byte-identical** to the one already verified in game.

**Confirmed the last unknown in that path:** font slices are a TrueType file behind a 24-byte header -
slice size is exactly font size + 24, and the sfnt signature sits at offset 24. A scan that expected
byte 0 found nothing, which is what failed first.

**Confirmed the container's own names for them:** `georgia`, `georgiab`, `georgiai`, `georgiaz`,
`goethebold` appear in the logged key table, and the recovered name tables name all eleven faces
(Georgia regular/bold/italic/bold-italic, Goethe bold, Tinos regular/bold, Mongolian Baiti, Consolas,
Hack, Font Awesome).

**Verified in game:** installed with the recovered mask, `title_mask_loaded` and `title_mask_found`
once each, frames 102/114/120 from a fresh process showing HYPR over LODR with the original fill.

**Fixed a hole in my own verification:** an earlier "pristine" check compared `OpenAL32.dll`'s hash but
never looked for `OpenAL32_real.dll`, so a leftover backup library silently blocked the next install
and a stale game process answered dump requests. Pristine now means: original hash, no backup library,
no mods directory, no `hyprlodr-*` artefacts.

**Tests:** face recovery 5/5, mask installer 3/3, installer safety 3/3.


## 2026-11-01 (fourth pass: the mask travels with the install)

**The replacement had a pipeline hole: the glyph mask was hand-copied.** Nothing in the repository
produced or delivered it, so a fresh install would have left the original title in place. It is now
part of the installer - `--title-mask PATH` places it, the manifest records its hash, uninstall removes
it only while the bytes match, a pre-existing mask is backed up and restored, and the copy is inside the
install's rollback.

**Three new scenarios** (`tools/tests/test_install_mask.py`) cover delivery, restore, and a missing
mask path failing before the proxy is touched: 3/3 pass, alongside the existing 3/3 installer
scenarios. The mask stays out of Git (`/assets/hyprlodr-title-mask.raw` is ignored) because it is
rendered from a face inside the user's own installation.

**Verified in game** with the mask delivered by installing: `title_mask_loaded` and `title_mask_found`
each once, `HYPR` over `LODR` at frames 483 and 512.

**Measured and set aside:** our lettering is already 94% packed - internal gaps of 9..19 px across the
831 px band against the original's zero - so per-word tracking is close to saturated and is not worth
further tuning.


## 2026-11-01 (third pass: the density fit is rejected, the exact-grid mask ships)

**Found a defect in what was shipping.** Chasing why the title looked slightly oversized, I measured
the shipped mask instead of trusting the generator's intent: it inked rows 5..476 and columns 0..852
with only 14 blank rows between the words, while the original inks rows 12..220 and 249..469, columns
12..842, with 28 blank rows. The cause was `--target-ink 0.428`, which scaled the glyphs 1.065 about
the centre to imitate the original's ink coverage - so the caps became 223/235 px against the measured
209/221 and the lettering ran past the margins it was supposed to respect.

**Decision: geometry over coverage.** Four letters cannot cover five letters' area at the same cap
height, so ink density is a consequence of the original's five-letter words, not a property worth
faking. The exact-grid mask now ships (ink 0.378, bands 12..220 and 249..469, columns 12..841, gap 28)
and `--target-ink` carries a help text saying it leaves the rects.

**Verified in game** at frames 120/300/480 and on demand at 491: `HYPR` over `LODR` inside the
measured margins with the full 28-row gap, the iridescent fill and animation unchanged, menu row
intact.

**Retracted mid-check:** a quick measurement appeared to show the original's caps swapped (221 top,
209 bottom). That was my own double flip - `mask_upright.bin` is already upright, so flipping it
inverted the bands. Measured unflipped, the original is 209 on top and 221 below, exactly as `RECTS`
already had it.


## 2026-11-01 (second pass: input falsified, resize survival confirmed)

**The focus hypothesis was wrong, and I retracted it.** I had written that Xvfb's lack of a window
manager meant the game never acquired focus. Re-running with `openbox` on `:99` showed `wmctrl -l`
listing `0x04600003  hyperdemon` and `xdotool getwindowfocus getwindowname` answering `hyperdemon`:
the game does hold X input focus. A real XTest click on OPTIONS, 24 incremental `mousemove_relative`
steps, and `Return`/`Tab`/`Right`/`Escape` all still left the title unchanged across frames 483, 553,
621, 505, 591 and 670. It is the game discarding injected events (as an XI2 raw-input client reading a
physical device would, since XTEST arrives on the virtual device), not the harness failing to focus it.

**The same window does obey the window manager.** Removing fullscreen and resizing to 1280x720 made
the engine re-lay out the title at the new size; restoring 1920x1080 re-laid it out again. So
`ConfigureNotify` is serviced while synthetic input is ignored, which rules out "the event queue is
not being read".

**Positive result: the replacement survives an engine-side relayout.** At both 1280x720 (frame 749)
and the restored 1920x1080 (frame 830) the title reads `HYPR` over `LODR`, and the log holds one
`title_mask_found`/`title_mask_loaded` pair with no `title_mask_stale`. The mask texture therefore
survived the resize and the per-frame re-upload held across it.

**Still blocked:** a true menu round-trip (title -> options -> title) needs input that this harness
cannot deliver.


## 2026-11-01 (exact geometry, on-demand capture, and a clean typeface verdict)

**The mask's geometry is now known exactly and matched.** Upper word rows 12..220 (cap 209), lower
rows 249..469 (cap 221), both columns 12..842, 28 blank rows between, margins 12/10/12/10. The
generator places the replacement on that grid; the earlier "gap rows 233..247" note is superseded.

**The typeface question is closed as unanswerable by silhouette.** At the original's own cap height
with tracking solved and no glyph distortion, Georgia 0.680, Mongolian Baiti 0.672, Tinos Regular
0.669 and Goethe Bold 0.663 all cluster, so filled-outline IoU cannot identify the face. Georgia
Bold stays as a good stand-in, not a proven match. Also retracted: the DEMON column of that run
reported 0.000 for most fonts because the font was sized from HYPER's bbox and DEMON's cap height
missed a 4px tolerance - a harness bug, not a result.

**On-demand frame capture works.** A serial number written to `hyprlodr-dump-request` dumps the next
presented frame, which fixed frame numbers cannot do because screen duration depends on load time.

**Blocked: menu navigation.** With the window found in the X tree and focused, an XTest click on
OPTIONS and a following Escape both left the title unchanged across frames 483/553/621 - Xvfb has no
window manager, so the game never takes focus. The round-trip stays unverified. What is verified:
the mask held over 140+ consecutive frames with its fill animating, and the hook now re-resolves the
texture if the engine drops it instead of uploading into a dead name.


## 2026-11-01 (the pack's fonts, and matching the title's weight)

**All eleven packed fonts are extracted and named**, after fixing a swapped length/offset pair in my
`name`-table reader: Georgia (regular, bold, italic, bold-italic), Goethe Bold, Mongolian Baiti,
Tinos (regular, bold), Consolas, Font Awesome 5 Free Solid, Hack. Fonts sit behind a 0x18-byte
payload header, which is why checking for TrueType magic at offset 0 alone found nothing.

**Which font drew the original title is narrowed, not proven.** Rendering `HYPER`/`DEMON` in each
font, normalising to each band's ink box and scoring IoU against the captured mask gives Georgia
Bold 0.677, Tinos Bold 0.638, Georgia 0.561, and drops to 0.35/0.33 for the sans and mono faces. So
the original is a bold serif of that genre and Georgia Bold is the best candidate, but a single-scale
silhouette comparison cannot identify it; that would need outline matching.

**The weight now matches.** The shipped mask inks 0.428 of the canvas; the generator fits its glyph
scale to a target ink (`--target-ink`, converged at 1.029) and measures 0.424 offline. In the running
game the mask texture reads back at 0.424 against the original's 0.428, with the mask still found by
signature (id 81) and frames 120/300/480 captured for the same session.


## 2026-11-01 (signature-based mask targeting, and animation preserved)

**The mask is now found by signature instead of by name, and the measurement justified it.** The
live renderer holds seven textures of exactly 853x480. Only one is the title mask: id 81, zero RGB,
mixed alpha. The others are uniformly clear or uniformly opaque (and one has non-zero colour), so a
size-only match would have hit id 112 and overwritten a texture the renderer owns. The loader scans
for size + zero colour + mixed alpha, logs `title_mask_found` with the name it chose, and uploads
the replacement mask there.

**Three frames from one session, 1920x1080:** frame 120 is the Sorath splash before the title
exists; frames 300 and 480 both show `HYPR` above `LODR` with the animated iridescent fill and the
menu row intact between them. Between 300 and 480 the lettering picks up a small rotation, which is
the game's own animated transform applied after the mask is sampled: the replacement inherits the
animation rather than reimplementing it.

**Process error worth keeping:** a build with a missing `OpenAL32_real.dll` failed, and because the
command was not fail-fast the run continued on the previous binary and produced misleading logs.
The cycle is now run with `set -euo pipefail` and the correct order (uninstall, build against the
pristine `OpenAL32.dll`, install), which is also why the build script's argument matters.

## 2026-11-01 (the title's mask texture, found in-game, replaced, and seen working)

**The title's lettering is a single 853x480 texture, and it is now read, replaced and verified in
the running game.** Reading every texture back at `wglSwapBuffers` found a texture whose RGB is
zero everywhere and whose alpha holds the `HYPER`/`DEMON` letterforms: two bands, a gap at rows
233..247, 42.8% inked, 256 alpha levels. The shader's own 854x480 window constant and the texture
size agree, which ties the decrypted source to the live renderer.

**Instrument, and a real negative.** The executable imports no GL library — no `opengl32`, no
`vulkan-1`, no `d3d11` — and through every patchable loader path asks only for
`wglChoosePixelFormat`, `wglSetPixelFormat`, `wglDescribePixelFormat`, `wglSwapBuffers`. I widened
the hook to patch `GetProcAddress` from any DLL and every direct `opengl32` import; the result was
still zero shader/program/draw events while frames rendered. Per-draw tracing is not available this
way for this engine, and that is now documented as measured rather than assumed. Texture readback
through the swap hook is, and it produced the mask.

**The replacement runs through the game's own title pass.** The mask is an ordinary texture the
engine never re-uploads, so writing our glyph mask into that same texture name makes `title_vs`,
the 854x480 window maths, the animated background texture, the `post_title` copy and the whole
post chain draw the new words. `tools/make_title_mask.py` renders `HYPR`/`LODR` in a packed font
into that exact layout (alpha = 255 - coverage, rows flipped for the upload path), and the loader
uploads it from frame 20 each session.

**Verified in the game:** at 1920x1080, frames 120 and 300 of one session show `HYPR` above
`LODR`, upright, filled by the animated iridescent background in the original oversized serif,
with `PLAY OPTIONS TRAINING CLIPS LEADERBOARDS EXIT` intact between them. The texture read back
after drawing contains our mask (ink 0.312 vs the original 0.428). An inverted first attempt is
recorded above the fix, since that is what established the upload path is bottom-up.

## 2026-11-01 (the title's fragment shader is read, and it corrects the geometry theory)

**The `title_vs` program is now fully understood, and it overturns tonight's earlier claim.** The
fragment shader is short and unambiguous: it centres an **854×480 window** horizontally by
`data.x`, aspect-corrects that window to the drawable, takes
`mask = vec3(1.0 - texture(tex_diffuse, uv1).a)`, takes `bg = texture(tex_diffuse3, uv0).rgb`,
clips the mask to the window, and writes `mask * bg` with alpha 1. So the title's lettering is the
**alpha channel of a texture, used inverted**, composited over a full-screen iridescent background
sampled with plain screen UVs. That is precisely the capture: the iridescent field is `bg`, and the
letters are the regions where `bg` is not multiplied in.

This supersedes "the title is geometry": there is no stored mask because the mask is a **render
target**. The same `gui` slice holds `gui_text_fs`, which multiplies a font mask into a colour and
writes it with alpha 0.5, so GUI text is rasterised offscreen and the title pass consumes such a
target. Glyph shapes can only come from the pack's eleven TrueType fonts.

**Method that made this tractable, worth keeping:** a `tag 8` slice starts with its own asset name
and `zlib.crc32(name) & 0x3ff` regenerates the key class it decrypts under, which is a closed
verification and named all 23 materials without pairing arrays to records. Getting there needed a
1.4× margin over the runner-up: a "readable text ≥ 20 chars" threshold alone admitted 800+
candidates per name, since random bytes are ~30%% printable.

**Consequence for the replacement (plan, not yet verified):** no new renderer is required. Keep
`title_vs`, the `post_title` copy and the background texture; change only the characters that are
rasterised into the title's alpha target, with the same font, the same inverted-alpha convention
and the same 854×480 window. The open question is the CPU-side string and font choice for that
pass.

## 2026-11-01 (the material set is named, and `title_vs` is found)

**The shader system is now a verified catalogue, and the title's program is identified.** A
`tag 8` slice begins with its own asset name, and `zlib.crc32(name) & 0x3ff` regenerates the key
class it decrypts under — a closed verification, so names no longer depend on pairing key arrays
to record arrays (that pairing is off by a shift and was the source of the earlier confusion;
`projection` is the shader at 306,711,296, not `hyper`). All **23 materials** came out of it:
`entity` (101,676 B), `post` (57,003), `tile`, `particle`, `dev`, `snake`, `warp`, `projection`,
`gem`, `lazer`, `hologram`, `portal`, `gui`, `owl`, `squid`, `light`, `retrieval`, `gem_old`,
`thorn`, `egg`, `skinning`, `hand`, `boss`.

The find that matters: **`gui` contains a program named `title_vs`**. Its vertex shader ignores
`world_matrix` and writes `gl_Position = vec4(in_position.xy, 0.0, 1.0)` — a full-screen quad —
and the paired fragment shader is `vec3 result0 = mask * bg;`. That is the title exactly as the
capture shows it: letterform mask multiplied against the iridescent background. `post` (1,465
lines) contains a matching `post_title_fs`, a plain `texture(tex_diffuse, vuv)` copy, inside the
post chain (`post_combine`, `post_blur_horz`, `post_glow_blur_vert`, lens flare, vignette, sobel).

Since the pack holds no image at all, the mask is built at run time — and the eleven `tag 4096`
fonts (`Georgia` ×4, `Goethe Bold`, `Mongolian Baiti`, `Tinos` ×2, `Consolas`,
`Font Awesome 5 Free Solid`, `Hack`) are the only glyph sources in the game. The next unknown is
therefore narrow and specific: which font the `title_vs` mask is rasterised from, and what tells
the renderer which characters to put in it.

Also corrected: an earlier attempt tonight scored records by "readable text" with a threshold of
20 characters, which admitted 800+ candidates per name because random bytes are ~30%% printable.
Requiring a 1.4× margin over the runner-up fixed it, and the CRC check then confirmed every name.

## 2026-11-01 (the pack is complete, contains no images, and the title is geometry)

**Three measurements, one of them a correction of my own.** First, the live record table is
*complete*: 863 non-overlapping spans tile 100% of the 306,794,369-byte pack, so every byte the
game can load is in a record we can read. Second, the pack contains **no image data at all** — no
`DDS `/`PNG`/`KTX `/`JPEG`/`RIFF`/`TGA` signature in any record, and no slice whose declared
width/height accounts for its bytes at 1–4 bpp (new tool `tools/re/find_textures.py`). Since the
eleven `tag 4096` slices are verifiably TrueType, the scan is reading payloads correctly. Third,
`HYPER`/`DEMON` appear in neither pack nor executable (ASCII or UTF-16), and the exe's Steam
interface is only `SteamAPI_Init`/`GetHSteamPipe`/`GetHSteamUser`/`IsSteamRunning`, so the words
are not fetched from Steam either (`appmanifest` does say `HYPER DEMON`, but the game never asks).

So the title's letterforms are **geometry** shaded by the `entity_vs`/`entity` material — the same
iridescent program with `spherical_proj`/`iri`/`crim_colour` — and a replacement must change glyph
geometry or its source, not a string or a bitmap.

**Two methodological corrections, both recorded rather than quietly dropped.** (a) A score based
on "how many of the letters H,Y,P,E,R,D,E,M,O,N a record's 1-char strings contain" matched 837 of
863 records: pure noise, discarded. (b) I then wrongly concluded that the content-based seed
inference was therefore unusable; a direct check says otherwise — on the 10 reachable named
slices it reproduces `zlib.crc32(name) & 0x3ff` **10/10**, including binary `tag 1`/`tag 2`
payloads, so the surveys above stand. `find_textures.py` now refuses unnamed records anyway,
because for them there is no way to *confirm* the seed.

Also confirmed this round from the captures themselves: the title is typography knocked out of the
iridescent field at an enormous scale (letters run off every edge), and the sibling `SORATH`
screen uses the same treatment, so this is the game's standard titling path rather than a
one-off.

## 2026-11-01 (the materials and the delivery path are both in hand)

**Two results, one negative, one constructive.** First, the probe's relaxed tag filter was
admitting junk: accepting any 16-bit tag turned 863 real slots into 4000 slots spread over 3377
"arrays" with tags such as `54240`, so the filter is back to the loader's measured dispatch set
`{1, 2, 4, 8, 0x20, 0x100, 4096}` (a clean run then reports 863 records in 238 contiguous runs).
With clean records, `zlib.crc32(name) & 0x3ff` recovers each slice's key class: the eleven
`tag 4096` records are the game's **fonts** (`Georgia` ×4, `Goethe Bold`, `Mongolian Baiti`,
`Tinos` ×2, `Consolas`, `Font Awesome 5 Free Solid`, `Hack`) keyed by `georgia`, `georgiab`,
`goethebold`, `monbaiti`, `tinos`, `tinosb`, …, and **`tag 8` is GLSL material source** — 22
shader programs specialised by defines (`SKINNED`, `LOBBY_DAGGER_HYPER2`, `GHOST`, `REVEAL`,
`OWLWORD`/`BIRDWING`, `CLIP_PLANES`, `ERODE`) sharing a uniform block with `spherical_proj`,
`accum_time`, `iri` and a ten-texture set. Index-aligning name arrays to record arrays is
therefore **wrong** (the 24-name skin array is off by a shift) and names must be assigned by
recovered key class; that supersedes the positional pairing in the previous entry.

Second, the **delivery path is proven**: `encrypt(decrypt(slice))` reproduces the shipped
ciphertext byte-for-byte on 8/8 real slices (1.4 KB – 10 MB, tags 1/2/8/4096), and the new
`tools/re/pack_patch.py` substitutes one re-encrypted same-length slice into a copy of the pack.
A no-change patch of `fract3` (7,039 B at 306,572,658) reproduces the installed 306,794,369-byte
pack with an identical sha256, which is only possible if the substitution is exact. The first
version of the patcher silently dropped the replacement bytes (output 7,039 B short); the hash
check caught it and it is fixed.

Third, the **negative that redirects the work**: scanning every record's decrypted payload in
ASCII, UTF-16BE and UTF-16LE finds `HYPER` only as the shader define `LOBBY_DAGGER_HYPER2`, and
`DEMON` nowhere — and neither word exists in the executable. So the title is not a stored string
and not a bitmap: it is glyph geometry shaded by `entity_vs`/`entity`, and a replacement must
change glyph content or geometry, not text.

## 2026-11-01 (the pack is decoded: fonts and material graphs read from real plaintext)

**The cipher is validated and the pack is readable, which unblocks the title.** The probe now
finds the resolver's parallel `std::string` key arrays (runs of `0x20`-byte elements that are
strictly ascending) and pairs them with record arrays by count: a 24-entry key array
`[6eye, bronze, …, hyper, hyper2, ..., unranked2]` matches a 24-entry record array whose offsets
chain exactly, so name → `(offset, size, tag)` is index-aligned. Validation is objective: for
each record of a newly seen `tag 4096` array, exactly **one** of the 1024 keystreams puts an
sfnt magic at `+0x18`, and a complete 22-table TrueType directory follows with every table
in-bounds — one-in-1024 per record, four records tested, so the cipher, the tables, the state
recurrence and the `0x18` payload header are confirmed against shipped data. Two new tags
appeared that the static dispatch reading did not show: **`tag 4096` is TrueType font** (11
records) and **`tag 8` is a named-graph type** whose `hyper`/`hyper2` payloads decrypt to graphs
mentioning `projection`, `retrieval`, `entity`. Consequence: the title's lettering is text drawn
with these fonts under an entity skinned by a `hyper` graph, so the replacement can use the
game's own glyph path and material. Also fixed in passing: both memory walks now step past
unmapped holes instead of ending the scan (measured: no change on this image, it was already
covering 282 regions / 722 MB). Open question: the `0x18` header decrypts to the same short string
across records while everything after it is valid — unexplained, not used.

## 2026-11-01 (the live asset record table is now readable)

New `native/win32/asset_probe.c` finds the game's 0x58-byte asset records in process memory
without needing any address from the analysis: it requires a valid
`(offset, size, tag)` slot plus three consecutive slots parsing. One instrumented run returned
400 records in **2 contiguous runs (longest 266)**, with offsets and sizes chaining exactly
(`232015452+3839395 = 235854847`, …), which proves the table is real. Tags seen: 266 × `0x100`
(audio) and 134 × `1`. A raw dump confirms the documented field positions, and shows the
`std::string` name field is **empty** — a scan of 722 MB / 282 regions found **zero** named
records, so names are not stored in the table and slices must be identified by content instead.
First content ranking (all 1024 keys, share of `0x00`/`0xff`) points at
`offset 297650309, size 258588, tag 1` (0.66) as a mask-like payload — unconfirmed. Also
corrected: the `ff 00 00 ff` window from the previous session sits inside a **tag-0x100 audio**
record, so the stencil reading is a hypothesis, not a fact.

## 2026-11-01 (the game runs again: dead X screen, then a virtual one)

**The blocker was environment, and it is solved.** Steam's client could not start its
UI: `Couldn't initialize SDL: The video driver did not add any displays`, because the
KWin Wayland session's Xwayland screen is **0x0** (`xdpyinfo`: `dimensions: 0x0 pixels`,
DPI `-2147483648`). With no X screen, Steam never attempted a logon, `-applaunch` did
nothing, and no capture tool could read the root window. Running everything on
`Xvfb :99 -screen 0 1920x1080x24 +extension GLX` fixes it: GLX gives GL 4.6
Compatibility through llvmpipe, Steam auto-logs in (`[Logged On, 4, 7]
[U:1:193590130]`), Proton starts, and the game reaches a 1920x1080 window with
`steamuser.log` = `Steam running..[berds]..login( )`. The proxy was then installed and
reported `anchors_matched` 8/8, `file_hook` installed over 39 modules, `gl_capture`
installed with 42 slots / 72 modules, and dumped a 1920x1080 frame. Three baseline
captures of the original title show enormous black glyph silhouettes (`HYPER` over
`DEMON`) in repeating vertical strips over an animated iridescent field, with the menu
row between them — i.e. a stencil/mask over a full-screen shader effect. The proxy was
uninstalled afterwards and `OpenAL32.dll` verified back to `396f8873…420b4`.

## 2026-11-01 (the pack cipher is reversed and implemented)

**`asset/asset` is encrypted, and its reader is now understood well enough to decrypt.** The pack measures exactly 8.000 bits/byte over its first MiB with all 256 byte values present and contains no plaintext names, so it is encrypted rather than compressed. `FUN_140213370` reads a slice and un-XORs it in place with a rolling 16-bit state driven by two 256-byte tables (RVA `0x361710`, `0x361810`) and by the *ciphertext* byte just produced; the per-slice seed is `FUN_140215280`, a table-driven reflected CRC-32 whose table at RVA `0x36f700` matches the standard `0xEDB88320` polynomial byte for byte, i.e. `zlib.crc32(name)`. `tools/re/pack_crypt.py` implements decrypt and encrypt, loads both tables from the local executable at run time (no game data in the repo), and passes a round-trip test over random payloads at several seeds and sizes. **Unverified:** correctness against real plaintext — that needs one known `(offset, size, name)` record, and brute-forcing 35,319 candidate names at offset 0 gave no plausible plaintext, as expected since offset 0 is not a named entry. A scan for a static index of monotonic `(offset, size)` pairs in the executable produced only code-byte false positives, so the index is built another way. Steam is still logged off (`LogonFailure Service Unavailable`, `alive=0`), so no runtime probe was possible.

## 2026-11-01 (records are typed slices of the asset stream; tag 0x100 is audio)

**The loader's second half was read to the end and it changes the shape of the problem.** `FUN_140212920` treats a record as `(offset u32, size u32, tag u32 at 0x40)`: it seeks the stream, `malloc`s `size` bytes, checks the name with `FUN_140215280`, reads the slice through `FUN_140213370` with that check, adds the payload check to the counter at `stream+0x168`, and dispatches the payload at `buffer+0x18` (so payloads have a 0x18-byte header) to one of five parsers: tag 1 → `FUN_140205ec0` (named sub-object graph, 404 lines), tag 2 → `FUN_1402330d0` (compact u16/u32 struct reader), tag 4 → `FUN_140210080` (length-prefixed string + ints), tag 0x20 → `FUN_140208790` (552-line text-format parser), tag 0x100 → `FUN_1401cd390`, which is **audio**: `alGenBuffers`/`alBufferData` with OpenAL mono/stereo formats and rate-based downsampling. So the lettering is addressed by name into the pack and materialised by a tag-selected parser; a genuine replacement is a same-tag payload served under the same name (the `_Fiopen` redirect path). Still unknown: the title's tag and payload layout — the pack remains undecoded and Steam is still logged off (`LogonFailure Service Unavailable`, launch attempt gave `alive=0`), so no runtime probe was possible.

## 2026-11-01 (asset record layout and loader dispatch, decompiled)

**The 0x58-byte asset record is now mapped, including the field that decides the title's form.** `FUN_140212040` is a field-by-field copy: u32 at `0x00`, u32 at `0x04`, u8 at `0x08`, u64 at `0x10`/`0x18`, a `std::string` at `0x20` (buffer/size/capacity `+0x20`/`+0x30`/`+0x38`), a **u32 type tag at `0x40`**, then u64 at `0x48`/`0x50`. `FUN_140213950` is the same sorted-map slot lookup as `FUN_14000c4d0` (used to write the loaded record back). The loader `FUN_140212920` dispatches on that type tag: type 1 lazily builds a 0x38-byte object plus a 0x50-byte sub-object (initialised with `+0x08 = 0x20`), caches it at `record+0x90` and gates re-entry on `(*(u32*)(obj+8) + 0x4c) & flags`; type 2 uses the cached pointer and allocates a 0x40-byte object. So the title's entity record carries a type tag and a name string — the texture-vs-mesh fork — while the remaining loader cases and the meaning of each tag are still unread. No in-game verification: Steam is still logged off (`LogonFailure …`), so the game exits before rendering.

## 2026-11-01 (asset resolver decompiled: (kind, name) -> 0x58-byte record)

**The lookup the lobby entities go through is now mapped.** `FUN_140211e70(ctx, kind, name, flags)` fetches a per-kind table with `FUN_140213b70(ctx + 0x128, …)`, binary-searches a sorted `std::string` key (SSO-aware: buffer `+0x20`, size `+0x30`, capacity `+0x38`, `memcmp`), calls the known "Can't find asset " builder `FUN_140213cb0` on a miss, and on a hit materialises a record via `FUN_140212040(&local, entryIndex * 0x58 + *table)` and loads it with `FUN_140212920(ctx, record, name, flags)`. The facility registry `FUN_14000c4d0` is the same sorted-key shape with insert-on-miss (`FUN_14000e630`, `FUN_14000e4a0`), which is why `FUN_140004ac0` can write its callback straight into the returned slot. So every entity/asset resolution is `(kind, name)` → 0x58-byte record in a per-kind table at `ctx + 0x128`; a title replacement means making the `entity_lobby_dagger_hyper*` lookup yield replacement content (asset redirect or record swap), not changing the lookup itself. No in-game verification: Steam is still logged off (`LogonFailure …`), so the game exits before rendering.

## 2026-11-01 (Ghidra decompiles the two helpers; one inference corrected)

**The named-facility registry is decompiled and an earlier reading is corrected.** `FUN_140004ac0(name, extra, callback)` lazily allocates a 0x40-byte global (`DAT_1403d91e0`), builds a `std::string` key from its second argument, obtains a slot via `FUN_14000c4d0(registry, key)` and stores the callback in it — a string-keyed name→callback table. So the `('shaders', 0x14036acb0, 0x1403da550)` site registers a **named facility**, not a directory to read from; that supersedes the directory wording. `FUN_140211bc0(ctx, name)` is a one-line typed wrapper over `FUN_140211e70(ctx, 4, name)` (kind tag 4), whose result the caller stores into 0x70-stride array slots (`0x68d8`, `0x6948`, `0x69b8`) — Ghidra typed the wrapper `void` while the caller consumes `RAX`, so the return is caller-derived. Replacement therefore means changing what the `entity_lobby_dagger_*` entries contain, not renaming anything. Ghidra recipe verified: pre-create the project directory, `-analysisTimeoutPerFile 300`, ~2 min for this PE. In-game verification still blocked: Steam remains logged off (`LogonFailure …`), the game exits without rendering.

## 2026-11-01 (title code sites: entities registered by name, shaders by directory)

**Static evidence places the title content outside the string space.** Both lobby entity names resolve to an identical registration block (`0x1401208a2`, `0x1401208e5`): build the literal, resolve through `0x140211bc0`, store into consecutive slots of an array with 0x70 stride (`0x68d8`, `0x6948`, `0x69b8`). The `shaders` literal is passed with a second string and a thunk (`0x1403da550`) into a registration routine at `0x140004ac0` from functions `0x1400019c8`/`0x14007199d`, and `skull_giga.vert` has **zero** RIP-relative references, i.e. it is reached through data. With `HYPER`/`DEMON` absent from the binary while `PLAY`/`OPTIONS`/… are literals, the title is entity/texture-driven — retitling cannot be a string patch. Linux `ctest` stays 6/6. In-game verification remains impossible while Steam is logged off (`LogonFailure …`): `SteamAPI_Init` fails and the game exits before rendering. Ghidra headless needed a pre-existing project directory; disassembly came from `objdump` over mapped byte slices.

## 2026-11-01 (root cause: Steam logged off, not the game)

**The launch blocker is an account state, not the game.** Steam's console log ends in `LogonFailure Service Unavailable` / `LogonFailure No Connection` followed by `LogOff()`, so `SteamAPI_Init` fails and HYPER DEMON exits deliberately within ~2 s: no window, no new crash dump, and the per-user `config` is never recreated — all three confirmed, with the loader uninstalled. The earlier prefix-rebuild advice is superseded; signing in to Steam is the fix. Static title work continued in parallel: the only `.vert` name in the binary is `skull_giga.vert`, and the lobby entity names resolve to two code sites (RVA `0x1208a2`, `0x1208e5`).

## 2026-11-01 (title evidence from the binary; startup blocker)

**The menu's labels are literals in the executable, but the title is not.** `strings` on `hyperdemon.exe` gives `PLAY`, `OPTIONS`, `TRAINING`, `CLIPS`, `LEADERBOARDS`, `EXIT`, while neither `HYPER` nor `DEMON` appears in ASCII or UTF-16. The binary also carries asset names `entity_lobby_dagger_hyper`/`…hyper2`, exactly one shader name (`skull_giga.vert`, twice), and font-dictionary vocabulary (`glyph-dict`, `FontName`, `FontMatrix`) without naming FreeType, HarfBuzz or stb_truetype. The working hypothesis is therefore that menu text is glyph-rendered while the enormous title is asset-driven — a wordmark texture or the `entity_lobby_*` meshes — which is exactly the distinction the texture census would settle.

**No in-game verification was possible: the game now exits ~2 s after launch with the loader uninstalled.** GL is healthy (`glxinfo` direct rendering, 4.6), Steam was restarted to a full 12-webhelper state, the game config was backed up and removed (never regenerated), all `hyprlodr-*` files were moved out, and the prefix registry is intact (4.7 MB `system.reg`). A minidump reader shows the earlier 04:53 dump as `0xC0000005` reading `0x90` in `ntdll+0x714e0` with 63 modules including `steam_api64.dll`; later launches write no dump at all, i.e. they die earlier than that crash. Remaining suspect is the compat prefix (`compatdata/1743850/pfx`), and recovery is Steam Verify followed by a prefix rebuild **after backing up `AppData/Roaming/HyperDemon`**, where scores and names live. Details: `docs/STARTUP_BLOCKER.md`, `docs/evidence/2026-11-01-startup-blocker.json`.

## 2026-11-01 (RenderDoc: closed route, and an environment regression)

**RenderDoc cannot capture this game, for three documented reasons.** `renderdoc` 1.46 has GL support, but `renderdoccmd capture` only wraps Linux executables; preloading `librenderdoc.so` into `proton run` aborts in Steam's Xalia helper (`No displays available`) so `hyperdemon.exe` never starts; `renderdoccmd inject` against a Steam-launched process printed usage and wrote no `.rdc`; and the game's context is **4.6 (Compatibility Profile)**, outside RenderDoc's core-profile GL replay support. No capture exists, so no title draw calls, shaders or textures were obtained this way (`docs/RENDERDOC_ATTEMPT.md`, `docs/evidence/2026-11-01-renderdoc.json`).

**The environment regressed during that work and it is not mod code.** After the direct `proton run` attempts, the game aborts within 3 s and writes `_crash_` to `steamuser.log` plus `crash_steamuser.dmp` **with the loader uninstalled** — `OpenAL32.dll` is the shipped file (`396f8873…`) with no manifest, and `hyperdemon.exe`/`asset/asset` are unchanged. The likely cause is two processes using the same `STEAM_COMPAT_DATA_PATH` (Steam's and the manual `proton run`). Recovery needs a Steam client restart and/or Verify integrity; neither was done here because both touch the user's session. The texture-census experiment written for the swap hook was therefore reverted rather than committed unverified.

## 2026-11-01 (OpenGL capture instrument, and the title's missing draws)

**The renderer is OpenGL, and we can now read its frames from inside the game.** `/proc/<pid>/maps` shows `opengl32.dll` → Mesa GLX with no Vulkan/D3D module, so the GLFW/WGL strings in the executable are the live renderer. A new capture in the proxy (`native/win32/gl_hook.c`) shims `KERNEL32!GetProcAddress` and `opengl32!wglGetProcAddress` across every module, and `wglSwapBuffers` gives a per-frame callback with a live context. In the real game it reported `GL 4.6 (Compatibility Profile) Mesa 26.2.5`, `AMD Radeon RX 9070 XT (radeonsi, gfx1201)`, viewport 1280×720 → 1920×1080, and the last draw's bound 2D texture; a 1920×1080 frame was read back with `glReadPixels` and is committed as `docs/evidence/2026-11-01-gl-frame.png`. **The title itself is still not reachable:** the engine requests no `gl*` name through either patched path (only four `wgl*` lookups), there are no delay imports, and re-patching changes nothing, so the engine's GL dispatch comes from somewhere else — most likely a compatibility/fixed-function path or a dispatch table installed before the proxy's DllMain. Nothing is drawn twice, patched, or replaced, and the original `OpenAL32.dll` was restored after the run.

## 2026-11-01 (mod-gated title overlay)

**Any mod now replaces the title screen; zero mods leave it alone.** The game's own title art is inside the undecoded container, so instead of blocking on that, the loader draws above it: a topmost, click-through, per-pixel-alpha Win32 window owned by the game process, sized to the game window, showing `hyprlodr-title.bmp`. It is created only when at least one mod initialized, never takes focus, and the game's real menu underneath stays clickable. Alpha comes from brightness, so the art's black background does not hide the game. Verified in the real game: with one mod the log recorded `shown` at 1288x754 and then `hidden` when `title_ms` elapsed (6/6 mods initialized, no crash record); with an empty mods directory the attach record says `title_overlay: not_started`, no overlay window exists and no title log is written. The screenshot in `docs/evidence/2026-11-01-title-overlay.png` shows the wordmark over the game's title screen. `tools/hyprl_title.py` converts the author's PNG to the BMP the loader reads, the CLI installer copies `assets/title.bmp` only when the game has none, and uninstall removes it. Fixing this exposed a real CRT bug: `fopen(..., "a")` truncated instead of appending, because the minimal runtime mapped every write mode to `CREATE_ALWAYS`.

## 2026-11-01 (asset interception found, and redirection proven)

**The asset path was `_Fiopen` all along.** Two earlier sessions concluded the game performed no asset I/O: the file hook patched `CreateFileA`, `fopen`, `fopen_s`, `_wfopen` and `_wfopen_s` in 39 modules and logged exactly one open (a Wine pipe). Reading `/proc/<pid>/fd` during a live session showed `asset/asset` genuinely held open, which turned the problem from "the game does not open files" into "we are hooking the wrong layer". Dumping the import table gave the answer: the game imports `MSVCP140.dll!?_Fiopen@std@@YEAU_iobuf@@PEBDHH@Z`, the C++ stream open. `std::ifstream` calls it inside MSVCP140, and that call reaches the UCRT by direct call, not through an import slot, so no amount of IAT patching below it can observe it. The hook also now installs from `DllMain` (phase 1: the executable's own slots; phase 2: the module walk), because the game starts opening files ~200 ms after load. With `_Fiopen` patched the log went from 1 record to 14, including `asset/asset` at t=215 ms and the per-user config, training, scores and names files.

**Asset redirection is real.** A probe mod (`r.asset`) redirected the `asset/asset` suffix to a hardlink of the original container inside its own state directory. The game's loader opened the replacement: `"requested":"asset/asset","redirected":true,"opened":"…\\hyprlodr-state\\r.asset\\asset.bin"`, 1 redirect published, 7/7 mods initialized, 14 opens logged, and the game ran normally — which it could not have done if the substitution were broken, since the hardlink has identical bytes. That is the first end-to-end proof that a mod can change the content the game loads (`docs/evidence/2026-11-01-asset-redirect.json`). Original `OpenAL32.dll` restored; Linux `ctest` 6/6 and the Wine suite stay green.

## 2026-11-01 (ABI 7: named game locations)

**Mods no longer hardcode RVAs.** ABI 7 publishes the locations the reverse-engineering pipeline recorded: `symbol_count()`, `symbol_name()` and `symbol_lookup()`. A lookup returns 0 only when the name is known *and* the live bytes at its RVA still hash to the FNV-1a 64 recorded for them, so a name can never resolve to data from a patched build; unknown names, missing images and hash mismatches are distinct refusals. The table is generated from the same PE analysis as the fingerprint (`game_anchors.h`), is copied into the loader, and is capped at 128 entries with 64-byte names. The proxy publishes the real eight anchors; the Linux host publishes test entries through `--symbol NAME@RVA:LEN`, and `--symbol-stale` hashes the bytes and then alters them, which is exactly the patched-build case. `example.game-info` now carries no RVA at all: it asks for `string:resource_path`, checks the returned length, reads, and independently re-hashes. In the real game it logged `symbol string:resource_path at rva 36f640 (12 bytes)` and then `resource path anchor verified at rva 36f640` with 8/8 anchors and 6/6 mods (`docs/evidence/2026-11-01-abi7-symbols.json`). Three new loader scenarios cover resolve, unknown name and tampered bytes; one real bug was caught on the way (the Windows host link line was missing `anchors.obj`). Linux `ctest` 6/6 (loader 41/41), Wine suite green, original `OpenAL32.dll` restored.

## 2026-11-01 (ABI 6: guarded writes)

**Mods can now change the game, and the host guarantees they cannot leave it changed.** ABI 6 adds `image_write()` and `image_restore()`. Writes fail closed: no verified build, no host write callback (the stand-in host under Wine), out-of-range or wrapping ranges, or an exhausted budget (32 patches / 4096 bytes) all refuse. Every patch saves the bytes it replaces, so a mod that fails initialization has its patches undone during rollback, `image_restore()` undoes them on demand, and shutdown restores whatever is left before the process exits — overlapping patches replay in reverse so the oldest bytes win. The Windows proxy performs the copy between two `VirtualProtect` calls because the image's pages are read-only. Linux coverage is four new scenarios: patch visible and restored (checked byte-for-byte through a new `--image-out` dump), refusal without an image, refusal out of range, and rollback of a failed mod's patch. In the real game the `i.writer` probe patched one byte at RVA `0x36f640`, saw it through a second read, restored it and re-verified the fingerprint with 8/8 anchors and 6/6 mods initialized (`docs/evidence/2026-11-01-abi6-write.json`). Linux `ctest` 6/6 (loader 38/38), Wine suite green, original `OpenAL32.dll` restored.

## 2026-11-01 (first data-reading mod)

**A mod read real game data and verified it.** ABI 5 was only proven with a PE header, which says nothing about game content. `examples/game_info.c` (id `example.game-info`) is the first shipped mod that reads the game at a fingerprinted offset: it gates on `build_matches()`, reads 12 bytes at RVA `0x36f640` through `image_read()`, recomputes the FNV-1a 64 hash itself and compares it with the verifier's constant. In the real game it logged `image 4214784 bytes verified` and `resource path anchor verified at rva 36f640` with 8/8 anchors and 7/7 mods initialized, so the bytes provably came from the running executable. It logs the hash, never the bytes. Two new loader scenarios run the same binary against synthetic images: 64 bytes is refused and 3.4 MB of `A` bytes is rejected by the mod's own comparison, which is the path a patched build takes. Linux `ctest` 6/6 (loader 34/34), Wine suite green, original `OpenAL32.dll` restored.

## 2026-11-01 (ABI 5)

**Mods can read the running game image.** ABI 5 adds `image_size()`, `build_matches()` and `image_read()` to the host API. Reads are refused unless the host proved the executable is the analysed build, and refused when the range is not entirely inside the image (the check also rejects a length that would wrap). The proxy supplies its mapped module plus the 8/8-anchor result; the Linux host supplies a file via `--image-file` so the semantics are tested against real fixtures: a 64-byte image returns exactly `0x1011121314151617` at offset 16, no image is refused, and offset 4096 is refused. Verified in the real game: the `i.image` fixture logged `image header 4d5a9000` with `build_matches=true` and 8/8 anchors, i.e. the bytes came out of the mapped `hyperdemon.exe` (evidence in `docs/evidence/2026-11-01-abi5-image.json`). Read-only by design: writing to the image is a separate capability with a much higher blast radius and is not part of this ABI.

## 2026-11-01 (continued)

**Rollback bug fixed and covered.** `state_remove()` deleted a key and the journal could not restore it, because rollback looked the key up in the table it had just been removed from. Rollback now re-inserts the previous value and owner when the entry is gone, and leaves the "drop what the mod added" path untouched. New scenario `state_remove_rolled_back_on_failure`: a failing mod removes another mod's key and adds its own, and the keeper verifies through the public ABI from `shutdown()` — which the loader runs after rollback — that the value came back and the failed mod's key did not survive.

**Installers are non-destructive.** Both `tools/hyprl_install.py` and `HYPRLODR.Core/LoaderInstaller.cs` now back up a mod file that already exists (`mods/.hyprlodr-backup`, recorded in the manifest), restore it on uninstall, and wrap the whole install in a rollback that puts the original library back and removes every partial artefact. Uninstall no longer deletes a mod whose bytes changed since installation; it reports it instead. Three Python scenarios (`install_suite`) and three manager scenarios (`loader_preserves_existing_mod`, `loader_install_rolls_back`, `loader_uninstall_keeps_changed_mod`) cover it.

**File hook hardened.** Open records use an atomic sequence number (concurrent game threads can no longer reuse one), the saved original pointer is published with a full fence before the slot is patched, the shim reads its pointers once into locals, a failed install closes its log handle, and redirect publication uses release/acquire so an unpublished table is invisible to a game thread mid-initialization.

**The regression that was not one.** Two sessions were lost to a Wine crash/hang that looked like a code regression: the test harness was fed `$GAME/OpenAL32.dll` while the loader was still installed in the game, so the proxy was copied in as its own forwarder target and looped into itself. Restoring the game first made the identical build pass. The test script still takes an arbitrary path, so the real fix is remembering to uninstall, not code.

**Wide-path hook and cross-module interception.** `_wfopen` is now patched too (UTF-16 path converted to UTF-8 for matching, replacement converted back). A real run with all four targets patched in the main module still logged zero opens, which exposed the next real gap: an import table belongs to a module, and the game's C++ runtime opens files through *its own* slots. The installer now walks `CreateToolhelp32Snapshot`, parses every loaded image and patches the same slots in each (23 modules under Wine, 39 in the game), skipping the proxy's own module so its start-up I/O is never redirected or logged as game activity. The shim is now proven live inside the game process: it captured a real `CreateFileA` open of `\\.\pipe\wine_plugplay`. Asset opens still do not appear, so the remaining candidates are kernelbase/`NtCreateFile` (no import slot) or the Steam API file layer — recorded in `docs/evidence/2026-11-01-file-hook.json`.

**File log is a timeline, and the long-session answer.** Every file record now carries `t_ms` since the hook installed and the opening thread id, so a capture can be read as a timeline rather than an unordered list. A 60 s live session (process alive at every 5 s check, 5/5 mods, all targets patched in 39 modules) recorded exactly **one** open — the Wine plug-and-play pipe at `t_ms: 35`. The shim is therefore live within ~35 ms of load and still sees no asset I/O in a full minute, which rules out the "assets load before the hook" theory and pins the remaining work to a non-import path.

**CreateFileW attempted and removed.** Adding a wide `WINAPI CreateFileW` shim looked like the last import-level gap: it patched 14 modules in the real game (`CreateFileW=patched(14) modules=39`) without disturbing start-up, but the real game still logged no opens. Verifying it needed a *second* module with its own `CreateFileW` slot, so `wprobe.dll` was added to the Wine stand-in — and calling through that patched slot terminated the process with no crash record and no file-log entry. Per the project rule that unproven and unsafe functionality is disabled rather than shipped, the target, its shim and the probe harness were deleted again; the evidence records the attempt and the symptom (`docs/evidence/2026-11-01-file-hook.json`). The remaining file hook is the verified set: `CreateFileA`, `fopen`, `fopen_s`, `_wfopen`, `_wfopen_s` across every loaded module.

**Measured the game's real file APIs.** `hyperdemon.exe` imports only `CreateFileA`/`ReadFile` from KERNEL32 and `fopen`, `fopen_s`, `_wfopen` from the UCRT API set. The hook was extended to the three narrow targets (symbol-resolved IAT patching, per-target failure policy) and installed successfully in the live game: `CreateFileA=patched fopen=patched fopen_s=patched`. Three real runs still logged **zero** opens, so the asset loader is almost certainly `_wfopen`, which is not intercepted yet. Redirection itself is verified end to end against the Wine stand-in (redirected bytes returned to the caller). Evidence: `docs/evidence/2026-11-01-file-hook.json`.

**Verified totals.** Linux `ctest` 6/6 suites (loader 29/29, package 19/19, installer 3/3, manager 32/32, anchors, asset probe); Windows/Wine suite green including forwarded exports, attach bootstrap, redirect bytes and the file log; original `OpenAL32.dll` restored byte-for-byte after every real-game run.

## 2026-10-31 (local session)

**Recovery.** Discovered the local checkout held the cloud agent's uncommitted work (no commits at all) and that `~/hyprlodr-recovery/hyprlodr-hJcCSK03` (commit `4c0bd6b`) differed from it only by the two GCC fixes (`dlsym` function-pointer cast, misleading indentation) plus `.gitignore`. Adopted those fixes verbatim, built, ran the existing lifecycle test, and committed the baseline as `f9a8a14`. No recovery data was modified or deleted.

**Loader (ABI 2).** Replaced the single-file MVP loader with the documented design: separate resolve/init phases, append-only descriptor extension, semantic-version constraints, conflicts, disabled mods, deterministic lexicographic ordering, seeded discovery shuffling, journaled rollback, `state_remove`/`is_loaded` host services, and sequential event numbers.

**Tests.** Replaced the single lifecycle script with a 23-scenario suite that runs the real host binary and asserts on the loader's own JSON event log. All 23 pass, including ABI mismatch, duplicate ids, cycles, diamond dependencies, rollback and disabled-dependency cases.

**Windows x64.** Added a self-contained target layer (minimal libc on kernel32, generated import library, `GetCommandLineA` entry shim) so the loader builds without any SDK, mingw or CRT. Built the five sample mod DLLs and verified them loading in order under Wine. Built the OpenAL32 proxy with 175 generated PE forwarders and verified forwarding plus the attach bootstrap in a stand-in process.

**Tooling.** Read-only diagnostics with an independent PE parser (verified against the real installation), transactional installer with hash manifest and rollback, proxy `.def` generator, Windows build and test drivers, full documentation set.

**Live cipher tables dumped from the real game.** The proxy copies the asset cipher's two 256-byte tables straight out of the running `hyperdemon.exe` image and writes them beside the attach record (`tables_dumped`, `table_t0_head` in the JSON; evidence as hashes in `docs/evidence/2026-10-31-tables.json`, never the bytes). They are byte-identical to the file's `.rdata` copy, which rejects the "tables are built at start-up" hypothesis and proves the cipher key material is already known. The Wine stand-in reports `tables_dumped=false` because those RVAs lie outside its image.

**State persistence proven across processes.** A fixture increments a counter in its state directory; a new loader scenario runs the host twice against the same root and observes 1 then 2, so ABI 3 storage is verified as durable rather than merely writable.

**Last-run report (manager).** `RunReportReader` turns the proxy's attach record and the loader's event log into one summary: stage, pid, whether the build fingerprint matched and at which anchor count, how many mods initialized, and the last error. The Game tab shows it, so in-game evidence is visible without opening JSON. Two scenarios cover the happy path and a corrupt/absent record.

**Mod profiles (manager).** `ProfileStore` captures the enabled set by name into `<game>/hyprlodr-profiles.json` and re-applies it by rewriting `disabled.txt`, so the native loader and the UI agree. Removing a mod does not break a profile: applying it reports the ids that are no longer installed. Three headless scenarios cover round-trip, apply-with-rewrite and tolerance of a corrupt profile file.

**Mod persistence (ABI 3).** `host_api.state_dir()` gives each mod a writable directory, reserved before `initialize()` and rooted at `<game>/hyprlodr-state` in the game. Two loader scenarios cover per-mod isolation and the no-root failure; the Windows host (ABI 3) and the Wine proxy path both pass. Uninstall leaves the directory alone: it is user data.

**Asset container triage.** `asset/asset` is 306 MB at 8.00 bits/byte with no strings and no zlib/bzip2/xz framing. Decompilation shows a C++ stream that opens the literal path and resolves entries after an internal header (`FUN_140212490` → `FUN_140110570`), with `FUN_140213cb0` building `"Can't find asset "`. Asset replacement stays blocked on understanding `FUN_140212c90`.

**Crash diagnostics (verified in the real game).** The proxy chains an unhandled-exception filter in front of whatever Steam installed and writes `hyprlodr-crash.json` with the exception code, faulting address and module offset. Real run 2026-10-31: the file never appeared, so the early exit is not an in-process fault; it is the process leaving normally. Under Wine a clean `proxy_client.exe` exit writes nothing either, which is asserted, so the filter cannot fire spuriously.

**Configurable start-up delay, and the first in-game mod init since the early sessions.** The bootstrap delay is now read from `<game>/hyprlodr-config.json` (`delay_ms`, clamped, default 2500, malformed input ignored) and echoed in the attach record, because the built-in delay was outliving this environment's process lifetime and hiding every mod result. Running with `delay_ms: 0` produced 35 events and **5/5 `initialize_ok`** in dependency order inside the real `hyperdemon.exe` — `z.foundation, a.palette, b.audio, m.overlay, 00.final` — with per-mod state directories created under the game, no failures, and the original `OpenAL32.dll` restored afterwards. The process still exits afterwards without an unhandled exception, so the remaining environment problem is the game dying, not mod loading.

**Build fingerprints (Level 4 groundwork, verified 2026-10-31).** `tools/re/analyze_pe.py` parses the PE, locates four short constants and the four RIP-relative instructions that reference them, and writes `native/win32/game_anchors.h`: RVA + FNV-1a 64 hashes only, no game content. `native/src/anchors.c` verifies them against a mapped image; the proxy parses the main module headers and reports `module_base`, `module_size`, `build_timestamp`, `build_matches` and per-anchor counts in `hyprlodr-attach.json`. Verified in the real game: 8/8 anchors matched at base `0x140000000`, timestamp `0x66ecd8b5`. Verified negatively under Wine: 0/8 and `build_matches=false` for a stand-in host, while the five mods still load. `anchor_test` covers the verifier in ctest.

**Real game (Level 2 verified).** Installed the loader into the local Steam installation, launched HYPER DEMON through Steam/Proton, and confirmed from inside the process that all five mods initialized in dependency order (`hyprlodr-attach.json` named `hyperdemon.exe`, 30 lifecycle events, five mod-authored log lines). Uninstalled afterwards and verified `OpenAL32.dll` is byte-identical to the shipped file. Evidence committed in `docs/evidence/`.

**Mod packaging (schema 1).** ZIP + `manifest.json` with defensive validation (traversal, absolute paths, backslashes, symlinks, duplicates, size and compression limits), an install registry for the future manager, and manifest/descriptor cross-checking through a new `hyprlodr-host --describe` mode that reads ABI descriptors without initializing mods. 19 package scenarios pass alongside the 23 loader scenarios.

**Desktop manager (Milestone 3).** Avalonia application over a tested core: Steam discovery (multiple libraries, manual override, Proton detection), loader install/remove with hash manifests and rollback, mod catalog with enable/disable and removal, package import, the exact load order the native loader will use, validation problems, and the event log viewer. Core services were covered by 16/16 headless scenarios when this milestone landed; the suite has since grown to 21/21. The UI was launched under Xvfb against a demo installation and captured in `docs/evidence/manager-ui*.png`. The screenshots exposed a real planner bug: a mod blocked by a disabled dependency was also labelled a cycle. Fixed with a regression test.

**Not done.** No game-facing capability yet (needs Ghidra/REA work on the executable and asset pack), no mod profiles or asset browsing, no audio-quality or gameplay verification, no remote catalogue.

Details: `docs/TESTING.md` (what is and is not verified), `docs/ROADMAP.md` (next steps).

**Start-up delay in the manager.** `LoaderConfig` reads and writes `<game>/hyprlodr-config.json`, preserving unknown keys and clamping to 0-60000; the Game tab exposes it next to the last-run panel, and a malformed file is reported rather than silently replaced by the proxy's default. Four headless scenarios cover the absent-file default, round-trip, clamping (on write and on read) and key preservation plus garbage handling.

**Run diagnosis in the manager.** `RunDiagnosis` combines the attach record, event log, install state and delay into one sentence in the Game tab. The important case is the one that hid a working loader: process ended during the delay, zero events. It says so and points at the delay, and when the delay is already 0 it explicitly does **not** blame the delay. Four headless scenarios cover not-installed, delay-caused, success, build mismatch, loader error and "ran but enabled nothing".

**Worked example mod, end to end.** `examples/uptime.c` is a real mod (versioned dependency, `state_dir` counter, capability, logging) rather than a fixture, built for both targets and packaged by the normal Windows build from `examples/package/manifest.json`. Validated and installed through `hyprl_package.py`, then loaded in the real game: 43 events, **6/6 `initialize_ok`** with `example.uptime` correctly ordered after `z.foundation`, and `<game>/hyprlodr-state/example.uptime/` holding `launches.txt` (1) and `last-run.txt` (1.0.0). Two build fixes came out of it: the Windows minimal CRT has no `fscanf`, and `-Wformat-truncation` rejects logging an unbounded path into a 256-byte buffer.
