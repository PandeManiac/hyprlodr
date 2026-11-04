# Reverse-engineering tooling

## Offline asset index and extraction

`hyprl_assets.py` turns a capture from the native in-game asset probe into a cached,
hash-associated record catalog. It preserves each distinct valid `(offset, size, tag)`
triple, duplicate evidence lines, encrypted hashes, tag names, and stable `rec-...` IDs.
Names remain unknown unless an explicit map from `name_records.py` is supplied or the
capture contains direct resolver `asset_mapping` events. Unique direct tree mappings are
labeled `verified`; a unique content-based map entry remains `inferred` because key-class
recovery alone is not a name-to-record proof. Repeated names across resolver tables are
qualified by stable table IDs.

```bash
GAME="$HOME/.local/share/Steam/steamapps/common/hyperdemon"
python3 tools/re/hyprl_assets.py scan --pack "$GAME/asset/asset" \
  --exe "$GAME/hyperdemon.exe" --evidence "$GAME/hyprlodr-assets.jsonl"
python3 tools/re/hyprl_assets.py list --pack "$GAME/asset/asset" \
  --exe "$GAME/hyperdemon.exe" --evidence "$GAME/hyprlodr-assets.jsonl"
python3 tools/re/hyprl_assets.py info fract3 --pack ... --exe ... --evidence ...
python3 tools/re/hyprl_assets.py extract fract3 --name fract3 --out /tmp/fract3.bin \
  --pack ... --exe ... --evidence ...
python3 tools/re/hyprl_assets.py info 'tbl-c6b9922fadd2:pain backup' --pack ... --exe ... --evidence ...
```

The cache defaults to the ignored local file `hyprlodr-assets-index.json` and is
accepted only when archive, executable, evidence, and optional name-map hashes still match.
`verify` checks cache identity, record bounds, and resolver tree-node accounting. Extraction
writes only to the requested destination and decrypts by logical name (or explicit `--key-class`); it never edits the
installed archive. A name duplicated across tables must use `table-id:name`; a unique
name can be used directly. Current capture coverage and unconfirmed candidates are
reported in `docs/RE_PACK.md`.

`asset_model.py` inspects the version-3 mesh section found in tag-1 graph payloads and
exports validated position/normal/UV0/index data as OBJ for Blender. See [`RE_MODEL.md`](../../docs/RE_MODEL.md)
for the verified `hand2` example and a local extraction workflow. Keep decrypted data and
exports under the ignored `.tmp/` directory; they are proprietary game content.

Read-only tooling for understanding a HYPER DEMON build. Nothing here modifies
the game, and nothing here is required to install mods: it exists so that any
claim about the executable is reproducible and any address-dependent work is
gated on a measured build identity.

## Workflow

```text
hyperdemon.exe
      |
      |  tools/re/analyze_pe.py            (PE parse, string + xref scan)
      v
native/win32/game_anchors.h  ------------->  proxy verifies at run time
      |                                       (hyprlodr-attach.json)
      |  RVA list
      v
Ghidra headless (tools/re/ghidra/DumpFunctions.java)
      |
      v
build-re/ghidra.log   (analysis material; never committed)
```

### 1. Derive or verify anchors

```bash
GAME=/path/to/steamapps/common/hyperdemon

# regenerate the committed header (hashes only, no game content)
python3 tools/re/analyze_pe.py "$GAME/hyperdemon.exe" \
    --json build-re/analysis.json \
    --header native/win32/game_anchors.h

# confirm the committed header still matches this build
python3 tools/re/analyze_pe.py "$GAME/hyperdemon.exe" \
    --verify native/win32/game_anchors.h
```

`--verify` exits non-zero on any mismatch. Run it after a game update before
trusting any RVA.

### 2. Decompile the function behind an anchor

```bash
mkdir -p build-re/ghidra
/opt/ghidra/support/analyzeHeadless "$PWD/build-re/ghidra" proj1 \
    -import "$GAME/hyperdemon.exe" -overwrite \
    -scriptPath "$PWD/tools/re/ghidra" \
    -postScript DumpFunctions.java 21257e 213ce6 \
    > build-re/ghidra.log 2>&1
```

Arguments are RVAs in hex without `0x`. For each one the script prints the
containing function, its size, its callees, and the decompiled body.

Ghidra auto-analysis of a 4 MB executable takes minutes; run it in the
background and tail `build-re/ghidra.log`.

### 3. Keep the results honest

`build-re/` is ignored by git on purpose:

- decompiled game code is derived proprietary content and must not be
  committed;
- only short identifiers, RVAs and hashes belong in the repository.

When a finding matters, record it in `docs/RESEARCH.md` as an observation with
the RVA it came from, distinguishing what was **observed** from what was
**inferred**.

### 3. Probe the asset container

```bash
python3 tools/re/asset_probe.py --exe "$GAME/hyperdemon.exe" --asset "$GAME/asset/asset"
python3 tools/re/asset_probe.py --selftest     # model check, runs in ctest
```

It reports entropy, standard framing probes, the cipher tables, and the outcome of decrypting the header. Today the real file prints `RESULT: decryption does not yield names` and exits non-zero. Do not treat the container as decoded until that line disappears.

## What the anchors mean

An anchor is `RVA + length + FNV-1a 64` of the bytes expected there. The proxy
verifies them against the mapped main module and writes the result into
`<game>/hyprlodr-attach.json`:

```json
"module_base":"0x0000000140000000",
"module_size":4214784,
"build_timestamp":"0x66ecd8b5",
"build_matches":true,
"anchors_total":8,
"anchors_matched":8
```

The proxy also copies the asset cipher's two 256-byte tables out of the running image into `<game>/hyprlodr-tables.bin` (reported as `tables_dumped`). Comparing that file with the executable's `.rdata` answers whether the tables are static or built at start-up: on the shipped build they are identical, so they are static.

`build_matches=false` means the running executable is not the analysed build.
Today mods still load in that case, because nothing depends on an address;
any future address-dependent capability must refuse to install itself instead.

## Proton input diagnostics

`proton_input.py` selects HYPER DEMON's app-specific Proton runtime from its
live process and matching wineserver. It carries that process's `DISPLAY`,
`XAUTHORITY`, and `HOME` into the helper launch; it never inherits the invoking
shell's display or X cookie. If `XAUTHORITY` is unset, it checks the matched
process's `HOME/.Xauthority`. The helper refuses ambiguous game processes, a
stale PID, a missing target display, or an active runtime that cannot be tied to
the matched process/prefix. It never invokes host `wine`, and refuses to run
when the wineserver for the target prefix belongs to a different Proton build.
The Windows probe filters by the
window title and the executable module enumerated from the window PID, verifies
foreground focus, uses client-relative coordinates, and can require a visible
post-input title change.

Build the helper and its local Win32 fixture with `tools/build_windows.sh`, then
run the isolated input test:

```bash
TASK_TMP="$PWD/.tmp/proton-input"
mkdir -p "$TASK_TMP"
TMPDIR="$TASK_TMP" TMP="$TASK_TMP" TEMP="$TASK_TMP" \
  tools/build_windows.sh .tmp/build-win "$HOME/.local/share/Steam/steamapps/common/hyperdemon/OpenAL32.dll"
TMPDIR="$TASK_TMP" TMP="$TASK_TMP" TEMP="$TASK_TMP" \
  python3 tools/re/proton_input.py --self-test
```

The self-test uses a disposable Proton prefix and Xvfb display; its fixture
passes only after receiving both a low-level mouse click and Return. All test
prefix data, logs, and generated binaries belong under ignored `.tmp/`. To
interact with a verified running game, use `python3 tools/re/proton_input.py
play`, `center`, or `enter`. Before sending input, the tool reports the selected
PID, display, Xauthority source, Proton runtime/version, Wine prefix, and input
target. Use `--display :99` to select another X server. If it requires a cookie,
also pass `--xauthority /path/to/cookie`; use `--xauthority none` only for an
unauthenticated display. A different display never inherits the game's cookie
implicitly. Normal input actions require one live, uniquely matched HYPER DEMON
process, and the tool checks that PID again immediately before launch.
