# First authenticated `hand2` replacement runbook

This procedure is for a user-initiated, legitimate Steam launch only. It does
not launch Steam or HYPER DEMON. Stop before staging unless the operator has
verified that the experiment cannot write to the normal Proton prefix, saves,
configuration, or Steam Cloud state. A local snapshot and a successful audit
are not proof of Cloud isolation or launch authorization.

All reports, captures, journals, and scratch files belong under ignored
`.tmp/`. Never use `/tmp`. Set the installed game path and scratch environment
first:

```bash
mkdir -p .tmp
export TMPDIR="$PWD/.tmp" TMP="$PWD/.tmp" TEMP="$PWD/.tmp"
GAME="$HOME/.local/share/Steam/steamapps/common/HYPER DEMON"
BUILD="$PWD/.tmp/hand2-runtime-build"
```

Build from the current committed source and the read-only shipped OpenAL input:

```bash
tools/build_windows.sh "$BUILD" "$GAME/OpenAL32.dll"
```

The deployment tool requires a clean committed source revision matching the
proxy build identity. Do not edit or replace the installed executable, archive,
or shipped OpenAL DLL. The deployment tool journals the files it changes and
copies the shipped OpenAL DLL to `OpenAL32_real.dll` before installing the
proxy.

## Before each phase

Use a distinct run ID and fresh process for each phase. Confirm that no game or
Steam process/session conflict exists and that the selected authenticated
launch will use the intended game files and writable prefix. The actual
executable, archive, prefix, Proton AppData, saves, and configuration paths
must be verified from process/runtime evidence; environment variables and
Steam launch settings alone do not establish them.

Review Steam Cloud behavior for this title before any launch. If Cloud could
sync the experiment's writes to the user's account, stop. Do not alter global
Steam or Cloud settings. Snapshot the installed game-specific writable data
and deployment state before staging. Discover the exact existing AppData path
from the user's Proton prefix and verify it directly:

```bash
USER_DATA="$HOME/.local/share/Steam/steamapps/compatdata/1743850/pfx/drive_c/users/steamuser/AppData/Roaming/HyperDemon"
SNAPSHOT_ID="hand2-before-<phase>-<unique-id>"
python3 tools/re/isolated_proton.py snapshot-auth-data \
  --game-root "$GAME" --user-data-root "$USER_DATA" \
  --snapshot-id "$SNAPSHOT_ID"
```

The snapshot is read-only and hash-verified. It is not a restore mechanism for
Cloud state. Also run the existing `authenticated-preflight` audit using its
documented arguments when a separate game copy and prefix are available; it is
audit-only and currently fails closed if separation cannot be established.
Do not bypass that result. If Steam cannot launch the controlled copy with a
verified separate prefix, or if paths/Cloud writes remain ambiguous, stop and
resolve that user-controlled boundary before deploying.

For every phase, verify the pinned files before proceeding:

```bash
sha256sum "$GAME/hyperdemon.exe" "$GAME/asset/asset" "$GAME/OpenAL32.dll"
```

Expected SHA-256 values, in command order:

```text
7612dd72076002b7c0cb5cab8f7340a399176ecedefe9d3b4d0353fbdac93611  hyperdemon.exe
2ce9dabb3251cb8d7994e2bda74056d344c06bf55b0d47b8645029b4687f99c7  asset/asset
8e0a069370f76de7084c39f23360311715f18677236d0e767c9af3f7992dbe64  OpenAL32.dll
```

The proxy build also records its source revision and artifact hashes:

```bash
cat "$BUILD/hyprlodr-proxy-build.json"
```

## Phase A: authenticated vanilla control

Start HYPER DEMON through the normal Steam UI with the original installation
and shipped OpenAL DLL. Do not deploy HYPRLODR. Confirm that the actual pinned
`hyperdemon.exe` reaches its title screen. Record the process identity, game
window, renderer state, actual executable/archive paths, prefix and game data
write paths, and whether any Steam Cloud activity occurs.

If the title screen is not reached, stop. Do not continue to Phase B. Record
the earliest specific startup failure; do not infer authentication failure
unless Steam API/runtime evidence supports it.

## Phase B: HYPRLODR original-geometry control

This phase installs the verified proxy and passive capture module with
replacement routing explicitly disabled. Use an original-size tag-1 archive
under `.tmp/` and the retained decrypted original payload:

```bash
RUN_B="$(date +%Y%m%d-%H%M%S)"
ORIGINAL_ARCHIVE="$PWD/.tmp/asset-proof/run3-original/asset.original-copy"
ORIGINAL_PAYLOAD="$PWD/.tmp/extracted/hand2.bin"
CAPTURE_B="$PWD/.tmp/hand2-runtime/phase-b-$RUN_B/hand2.capture"
JOURNAL_B="$PWD/.tmp/hand2-runtime/phase-b-$RUN_B/deployment"
mkdir -p "$(dirname "$CAPTURE_B")"
python3 tools/hand_capture_deploy.py preflight \
  --game-dir "$GAME" --build "$BUILD" --archive "$ORIGINAL_ARCHIVE" \
  --expected-payload "$ORIGINAL_PAYLOAD" --capture-output "$CAPTURE_B"
```

Proceed only if the report says `ok: true`, mode `original-capture`, and
confirms the pinned executable/archive/OpenAL and current clean build. Then
make the reversible deployment:

```bash
python3 tools/hand_capture_deploy.py deploy \
  --game-dir "$GAME" --build "$BUILD" --archive "$ORIGINAL_ARCHIVE" \
  --expected-payload "$ORIGINAL_PAYLOAD" --capture-output "$CAPTURE_B" \
  --journal "$JOURNAL_B"
```

The tool configures the asset redirect and capture mode, sets
`experimental_record_route` to false, and journals any existing proxy,
forwarder, mods/configuration, and mode files it replaces. Launch through the
normal authenticated Steam UI. For phases B and C, the proxy's early record
diagnostics require `HYPRLODR_FIRST_LOAD_DIAGNOSTICS=1` and
`HYPRLODR_EARLY_RECORD_HOOK=1`. If using Steam's per-game Launch Options, the
operator must record the existing value, temporarily prefix it with
`env HYPRLODR_FIRST_LOAD_DIAGNOSTICS=1 HYPRLODR_EARLY_RECORD_HOOK=1 %command%`,
and restore the original value immediately after the run. Do not change global
settings; if the prior value cannot be preserved, stop. Repository tooling does
not edit Launch Options. Record the actual Wine game PID from process evidence
and capture the hand draw while entering gameplay.
Validate the capture:

```bash
python3 tools/re/hand_capture.py "$CAPTURE_B" --original "$ORIGINAL_PAYLOAD" --json \
  "$PWD/.tmp/hand2-runtime/phase-b-$RUN_B/analysis.json"
```

Require a valid original 238/1,188 capture and normal rendering. Correlate the
capture with the run's attach/startup/file diagnostics and record their run IDs.
If startup, archive open, diagnostics, or the original capture fails, stop
before Phase C.

When the process has exited, collect diagnostics before restoring:

```bash
STARTUP_LOG="$(find "$GAME" -maxdepth 1 -name 'hyprlodr-startup-*.jsonl' -print -quit)"
FILES_LOG="$(find "$GAME" -maxdepth 1 -name 'hyprlodr-files-*.jsonl' -print -quit)"
python3 tools/re/first_load_diagnostics.py coverage \
  --startup "$STARTUP_LOG" \
  --files "$FILES_LOG" \
  --attach "$GAME/hyprlodr-attach.json" \
  --output "$PWD/.tmp/hand2-runtime/phase-b-$RUN_B/coverage.json" \
  --process-exited
python3 tools/hand_capture_deploy.py restore --journal "$JOURNAL_B/deployment.json"
```

Confirm the restore report, verify restored file hashes against the journal,
and keep runtime evidence in `.tmp/`. Do not reuse this journal for another
phase.

## Phase C: enlarged native replacement

Use a separate fresh process, a new snapshot, capture path, and deployment
journal. First validate the retained append-only archive and route manifest
against the pinned executable and source archive:

```bash
RUN_C="$(date +%Y%m%d-%H%M%S)"
APPENDED="$PWD/.tmp/archive-tests/asset-hand2-appended"
MANIFEST="$PWD/.tmp/archive-tests/hand2-override.json"
REPLACEMENT="$PWD/.tmp/roundtrip/hand2-blender-added.bin"
CAPTURE_C="$PWD/.tmp/hand2-runtime/phase-c-$RUN_C/hand2.capture"
JOURNAL_C="$PWD/.tmp/hand2-runtime/phase-c-$RUN_C/deployment"
mkdir -p "$(dirname "$CAPTURE_C")"
sha256sum "$GAME/hyperdemon.exe" "$GAME/asset/asset" \
  "$APPENDED" "$MANIFEST" "$REPLACEMENT"
```

The repository's append tool creates and validates at build time; the retained
archive is validated here by the deployment preflight, which checks the exact
tag-1 record, appended archive prefix, ciphertext decryption, payload
hash/topology, and manifest:

```bash
python3 tools/hand_capture_deploy.py preflight \
  --game-dir "$GAME" --build "$BUILD" --archive "$APPENDED" \
  --expected-payload "$REPLACEMENT" --route-manifest "$MANIFEST" \
  --capture-output "$CAPTURE_C"
```

Proceed only if the report is `ok: true`, mode `append-route`, with 239
vertices and 1,191 indices. Then journal and deploy:

```bash
python3 tools/hand_capture_deploy.py deploy \
  --game-dir "$GAME" --build "$BUILD" --archive "$APPENDED" \
  --expected-payload "$REPLACEMENT" --route-manifest "$MANIFEST" \
  --capture-output "$CAPTURE_C" --journal "$JOURNAL_C"
```

The proxy configuration enables only the validated route, and the asset
redirect points to the same appended archive so the archive-open gate observes
the same file. Launch through the normal Steam UI with the temporary diagnostic
variables described in Phase B. At the title screen, identify the actual
`hyperdemon.exe` Wine PID from `ps -eo pid,lstart,args`, enter that verified PID,
and start the existing read-only asset tracer in the background before entering
gameplay:

```bash
read -r -p 'Verified hyperdemon.exe Wine PID: ' GAME_PID
python3 tools/re/asset_trace.py capture --pid "$GAME_PID" --duration 120 \
  --out "$PWD/.tmp/hand2-runtime/phase-c-$RUN_C/asset-trace.jsonl" &
TRACE_PID=$!
# Enter gameplay now; after the capture window finishes:
wait "$TRACE_PID"
```

The trace is a late attach and cannot prove events that preceded attachment;
use startup diagnostics for the early route/archive ordering and treat missing
late-trace events as unknown. Require, in order, a
matching process/run identity, proxy attach and executable fingerprint,
dormant hook installation, manifest validation and route publication, archive
gate `READY`, appended archive open, exact tag-1 `hand2` selection and
replacement descriptor offset/size, successful native loader return, and
parser counts 239/1,191 with stride 72. Then require a passive capture with
17,208 VBO bytes, 4,764 IBO bytes, valid indices, and a 1,191-index
`GL_TRIANGLES`/`GL_UNSIGNED_INT` draw. The retained replacement preserves the
original 1,188-index prefix and appends `[0, 1, 238]`; confirm that final
triangle in the captured index data. The compiled synthetic fixture uses a
different deterministic final triangle, `[238, 1, 2]`. Analyze the real capture:

```bash
python3 tools/re/hand_capture.py "$CAPTURE_C" \
  --modified "$REPLACEMENT" \
  --json "$PWD/.tmp/hand2-runtime/phase-c-$RUN_C/analysis.json"
```

Index equality and topology must be evaluated separately from vertex-byte
equality because posing can transform vertices. Correlate timestamps, buffer
IDs, index contents, native parser events, and the exact route record; dimensions
alone do not establish resource identity. Inspect a gameplay frame to decide
whether the additional triangle is visible. This is a separate result from
successful parsing, upload, or draw submission.

If any required phase-C event is missing or contradictory, stop and preserve
the evidence; do not automatically retry or infer success from a partial
capture. After the game exits, collect coverage and traces, then restore:

```bash
python3 tools/re/first_load_diagnostics.py coverage \
  --startup "$(find "$GAME" -maxdepth 1 -name 'hyprlodr-startup-*.jsonl' -print -quit)" \
  --files "$(find "$GAME" -maxdepth 1 -name 'hyprlodr-files-*.jsonl' -print -quit)" \
  --attach "$GAME/hyprlodr-attach.json" \
  --output "$PWD/.tmp/hand2-runtime/phase-c-$RUN_C/coverage.json" \
  --process-exited
python3 tools/hand_capture_deploy.py restore --journal "$JOURNAL_C/deployment.json"
```

Verify all journaled installation paths match their pre-deployment state and
that only expected run artifacts remain under `.tmp/`. The original executable
and archive are never deployment targets. If the Steam launch uses a different
copy, prefix, or writable path than verified, stop and restore without
continuing the experiment.
