# HYPRLODR

An unofficial, extensible mod loader and mod management platform for **HYPER DEMON** (SORATH, Steam app `1743850`).

HYPRLODR is two things: a native plugin runtime that gets third-party code loaded inside the game, and the tooling that makes installing, validating and testing that code safe and repeatable.

The original game files are never modified destructively: installing the loader renames the shipped `OpenAL32.dll` to `OpenAL32_real.dll` and records hashes so the installation can be verified and reverted.

## Status

| Level | Description | Status |
|---|---|---|
| 1 | Five independently compiled mods load, respect dependency order, share state, shut down in reverse | **Verified** (Linux + Windows x64 under Wine) |
| 2 | Diagnostic mod executes inside the real HYPER DEMON process | **Verified 2026-10-31** — `hyperdemon.exe` launched through Steam/Proton initialized all five mods in-process; evidence in `docs/evidence/` |
| 3 | Desktop mod manager | **Implemented** — Avalonia app; 16/16 headless core scenarios; UI captured in `docs/evidence/manager-ui*.png` |
| 4 | Mods change the game | Not started |
| 5 | Asset replacement | Not started (asset pack documented, format unknown) |

Details and evidence: `docs/RESEARCH.md`, `docs/TESTING.md`, `docs/ROADMAP.md`.

## What works today

- **Versioned C ABI (`hyprlodr/plugin.h`, ABI 2)** with a single export per mod.
- **Deterministic loader**: discovery, descriptor validation, semantic-version constraints, conflicts, disabled mods, lexicographic tie-breaks, cycle/missing-dependency detection, partial-failure rollback, reverse-order shutdown.
- **Structured event log** (JSON Lines) with sequential event numbers, plus a JSON summary from the host.
- **Five mandatory sample mods** compiled independently through the public header.
- **23-scenario test suite** driving the real host binary, including randomized discovery order.
- **Windows x64 build** with no Windows SDK, mingw or CRT dependency: the loader ships its own minimal libc on kernel32 and generates its import library with `llvm-dlltool`.
- **OpenAL32 proxy** with 175 generated PE forwarders to the renamed original, so audio behaviour is untouched.
- **Desktop manager** (Avalonia): Steam discovery, loader install/remove with hash verification, mod list with enable/disable, package import, load-order preview, validation problems, log viewer.
- **Versioned mod packages** (ZIP + `manifest.json`, schema 1): defensive validation, manifest/descriptor cross-checking, install registry, pack/validate/install/list/remove CLI.
- **Real game integration**: with the proxy installed, HYPER DEMON starts normally and mods initialize inside the game process (29 s session, 5/5 mods, original library restored by uninstall).
- **Read-only diagnostics** (`tools/hyprl_diagnose.py`) that locate Steam libraries on Linux/Windows, hash the shipped binaries, parse PE headers/imports/exports, detect Proton, and report existing HYPRLODR state.

## Build

Linux/macOS (native tests):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Windows x64 cross-build (clang + lld + llvm-dlltool only):

```bash
tools/build_windows.sh build-win /path/to/HYPER DEMON/OpenAL32.dll
tools/test_windows.sh build-win /path/to/HYPER DEMON/OpenAL32.dll   # requires wine
```

The second argument is optional; without it the proxy DLL is skipped.

## Diagnostics and installation

```bash
python3 tools/hyprl_diagnose.py                     # read-only report
python3 tools/hyprl_diagnose.py --json report.json
python3 tools/hyprl_install.py status
python3 tools/hyprl_install.py install --game-dir "/path/to/HYPER DEMON" --build build-win
python3 tools/hyprl_install.py uninstall --game-dir "/path/to/HYPER DEMON"

# mod packages
python3 tools/hyprl_package.py pack my-mod/ out/my-mod.zip
python3 tools/hyprl_package.py validate out/my-mod.zip
python3 tools/hyprl_package.py install out/my-mod.zip --mods-dir "<game>/mods" --host build-win/hyprlodr-host.exe
python3 tools/hyprl_package.py list --mods-dir "<game>/mods"

# desktop manager
dotnet run --project manager/HYPRLODR.Manager
```

Manager core tests (headless, no display needed):

```bash
tools/test_manager.sh
```

`install` refuses to overwrite files it did not create, records SHA-256 hashes in `hyprlodr-install-manifest.json`, and never deletes the original library.

## Layout

```text
native/include/hyprlodr/plugin.h   public mod ABI
native/src/loader.{c,h}            loader library
native/src/host.c                  synthetic host (JSON summary + event log)
native/win32/                      Windows target layer (minimal libc, proxy, entry shim)
native/tests/loader_suite.py       23-scenario loader suite
native/tests/fixtures/plugin.c     configurable error-case fixture mod
samples/                           the five mandatory sample mods
tools/                             diagnostics, installer, package tooling, proxy .def generator, builds
manager/                           Avalonia desktop app (HYPRLODR.Manager) + core services + self-tests
docs/                              architecture, research, API, testing, security, roadmap
```

## Safety

Native mods are trusted code inside the game process: they can crash, corrupt state, or do anything the game can. There is no sandbox. HYPER DEMON has competitive scoring; modded sessions must not be treated as leaderboard-safe. See `docs/SECURITY.md`.
