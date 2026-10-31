# Research log

Every statement is tagged **Observed** (direct binary evidence), **Inferred** (strongly suggested), or **Hypothetical** (needs verification). Nothing here should be read as a statement about the engine's internals that the evidence does not support.

Local installation examined: `/home/berds/.local/share/Steam/steamapps/common/hyperdemon` (Steam library `/home/berds/.local/share/Steam`, app `1743850`, `buildid 15765018`, `StateFlags 4`).

## Binaries

| File | Size | SHA-256 |
|---|---|---|
| `hyperdemon.exe` | 4,175,360 | `7612dd72076002b7c0cb5cab8f7340a399176ecedefe9d3b4d0353fbdac93611` |
| `OpenAL32.dll` | 1,795,072 | `396f88738ce20905e933cb7d8c696351e41bd7e72e80b8cfdadf20ab613420b4` |
| `steam_api64.dll` | 262,944 | `a178f19a516023efc3cc30b1e90fbeda838d08d4f1fa006b895608d67fa60eac` |
| `discord_game_sdk.dll` | 3,891,512 | `527768710ddb0953fce5eb1700c2566b6451135d76f1d0610b63907cd5ba94c5` |
| `asset/asset` | 306,794,369 | see `tools/hyprl_diagnose.py --json` |

All hashes and sizes were produced by `tools/hyprl_diagnose.py` against the local installation. Re-run it after a game update; these values change and the loader installation manifest records what was actually present at install time.

## hyperdemon.exe

**Observed** (`llvm-objdump -p`, PE parsed independently by `tools/hyprl_diagnose.py`):

- PE32+ (`Magic 020b`), `coff-x86-64`, image base `0x140000000`, subsystem 2 (Windows GUI), `DYNAMIC_BASE`, `NX_COMPAT`, `HIGH_ENTROPY_VA`.
- Linker 14.16, timestamp `2024-09-19 21:06:45`, `SizeOfCode 0x313000`.
- Exports: only `AmdPowerXpressRequestHighPerformance` and `NvOptimusEnablement` (ordinals 1–2) — the standard hybrid-GPU markers, no engine API surface.
- DLL imports (33): `dbghelp.dll`, `steam_api64.dll`, `WINMM.dll`, `OpenAL32.dll`, `discord_game_sdk.dll`, `WS2_32.dll`, `WLDAP32.dll`, `CRYPT32.dll`, `ole32.dll`, `GDI32.dll`, `ADVAPI32.dll`, `USER32.dll`, `SHELL32.dll`, `MSVCP140.dll`, `MFReadWrite.dll`, `MFPlat.DLL`, `SHLWAPI.dll`, `KERNEL32.dll`, `IMM32.dll`, `VCRUNTIME140.dll`, and twelve `api-ms-win-crt-*` forwarders.
- Functions imported from `OpenAL32.dll` (26, by name): `alBufferData`, `alBufferiv`, `alDistanceModel`, `alGenBuffers`, `alGenSources`, `alGetError`, `alGetProcAddress`, `alGetSourcef`, `alGetSourcei`, `alGetString`, `alIsExtensionPresent`, `alListenerf`, `alListenerfv`, `alSource3f`, `alSource3i`, `alSourcePlay`, `alSourceRewind`, `alSourceStop`, `alSourcef`, `alSourcei`, `alcCreateContext`, `alcGetIntegerv`, `alcGetProcAddress`, `alcMakeContextCurrent`, `alcOpenDevice`.

Notes on evidence quality: the export table and import table were read twice, once with `llvm-objdump` and once with an independent Python PE parser in `tools/hyprl_diagnose.py`, and agree.

**Inferred:** because `OpenAL32.dll` is a static import, the Windows loader resolves the file from the application directory during process initialization, before the executable's entry point. A same-named replacement in the game directory is therefore loaded without patching, hooking or code caves. This is the basis of the integration mechanism.

**Inferred:** `MSVCP140.dll`/`VCRUNTIME140.dll` imports indicate a C++ toolchain (MSVC 2015+ ABI) — consistent with a C++ engine, but no conclusion about engine identity is drawn.

**Hypothetical (not verified locally):** earlier ad-hoc experiments reported `glfw`-related initialisation and internal asset identifiers such as `s_player`. Those strings were not re-checked in this session and no symbol, offset or structure name from those notes is used anywhere in HYPRLODR. Treat them as unverified leads for the future asset workstream.

## OpenAL32.dll

**Observed:** 1,795,072 bytes, x86-64 PE, **175 named exports** covering the standard OpenAL 1.1 `al*`/`alc*` surface plus `Java_net_java_games_*` JNI helpers. The game imports 26 of them by name; no ordinal imports were observed from the game.

**Inferred:** a complete, name-compatible forwarder DLL is sufficient to replace it. Ordinal-only imports would need explicit `@ordinal` entries; none are required by this build.

## Installation layout

**Observed:**

```text
hyperdemon/
  hyperdemon.exe
  OpenAL32.dll
  steam_api64.dll
  discord_game_sdk.dll
  asset/asset                     (306,794,369 bytes, single packed file)
  OST/1 Hyper.mp3, OST/2 Demon.mp3, OST/EXTENDED SOUNDTRACK URL.txt
```

**Observed:** `steamapps/compatdata/1743850/` exists with a populated `pfx`, and both `Proton - Experimental` and `Proton Hotfix` are installed in `steamapps/common`. The game is therefore being run through Proton on this machine.

**Hypothetical:** the packed `asset/asset` container almost certainly holds textures, meshes, audio and shaders. Its format has **not** been analysed in this session; no offsets, tables or identifiers are claimed. Asset replacement should start from a verified resource-lookup boundary, not from guesses.

## Integration mechanism assessment

| Mechanism | Evidence | Decision |
|---|---|---|
| DLL proxying of statically imported `OpenAL32.dll` | Static import table observed; 175 exports enumerated | **Chosen.** No addresses, no hooks, update-resilient, auditable. |
| Native plugin bootstrap inside the proxy | Proxy DLL can carry arbitrary code | **Chosen** as the transport for the loader. |
| Hooking engine functions at fixed addresses | No signatures established | Rejected for now: fragile, unverifiable, and unnecessary to load code. |
| Asset redirection | Asset container unanalysed | Deferred until the container format is understood. |
| Steam API patching | Would touch DRM-adjacent surface | Rejected. Nothing in HYPRLODR patches Steam or integrity checks. |

## Tooling available

**Observed:** `clang 23.1.3`, `lld-link`, `llvm-dlltool`, `cmake`, `ninja`, `ghidra 12.1.2` (`/usr/bin/ghidra`), `rea` CLI 6.7.0, `wine 11.20 (Staging)`, `.NET SDK 10.0.113`.

**Observed:** `rea doctor` reports `ghidra` healthy (12.1.2, Java 21, headless available) while overall `healthy: false` because the Hopper provider and several client integrations are missing. REA was not used for the findings above: every claim in this file was produced with `llvm-objdump` and the project's own PE parser, which are reproducible locally. REA/Ghidra remains the intended tool for the asset and renderer workstream, where decompiled cross-references are needed.

## Known limitations

- The real game has not been launched with HYPRLODR installed in this session; see `docs/TESTING.md`.
- Nothing is known yet about shader, mesh or texture formats.
- Address-dependent techniques are deliberately absent, so no compatibility fingerprint exists to check against future builds.
