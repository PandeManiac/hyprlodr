# Reverse-engineering tooling

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

`build_matches=false` means the running executable is not the analysed build.
Today mods still load in that case, because nothing depends on an address;
any future address-dependent capability must refuse to install itself instead.
