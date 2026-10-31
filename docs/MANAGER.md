# Desktop manager

`manager/HYPRLODR.Manager` is an Avalonia desktop application (Windows, Linux, macOS) that wraps the native loader and package tooling. It is the Milestone 3 deliverable: install, inspect, enable, disable and remove mods without touching DLL files by hand.

```bash
dotnet run --project manager/HYPRLODR.Manager
# or point it at a specific installation
HYPRLODR_GAME_DIR="/path/to/steamapps/common/hyperdemon" dotnet run --project manager/HYPRLODR.Manager
```

## Screens

**Game** — detected Steam installations (multiple libraries supported), manual folder override, build id, Proton prefix/version, loader state with file hashes, and the install/remove/launch actions.

**Mods** — installed mods with version, enabled state and status; per-mod metadata, dependencies and conflicts; the exact load order the native loader will use; and validation problems (missing dependency, disabled dependency, version mismatch, cycles, duplicates).

**Logs** — the loader's JSON Lines event log, filterable by event, mod or message.

## Core services

The UI is deliberately thin. Everything testable lives in `manager/HYPRLODR.Core` and is exercised headlessly by `manager/HYPRLODR.SelfTest` (15 scenarios, wired into ctest as `manager_suite`):

| Service | Responsibility |
|---|---|
| `SteamLocator` | Steam roots, `libraryfolders.vdf` parsing (modern and legacy formats), `appmanifest` parsing, Proton detection, manual override |
| `LoaderInstaller` | Install/uninstall the proxy with a hash manifest, refuse interrupted or tampered installs, restore the original byte-for-byte |
| `ModCatalog` | Read binaries + registry + `disabled.txt`, reproduce the loader's topological ordering and validation rules |
| `ModInstaller` | Enable/disable via `disabled.txt`, remove with hash verification |
| `PackageInspector` | Schema-1 package validation and import, optional descriptor cross-check through `hyprlodr-host --describe` |
| `EventLogReader` | Parse the loader's JSONL events, tolerating a partially written final line |

Ordering in the UI mirrors the native loader exactly: repeated selection of the lexicographically smallest ready mod, with the same failure classification. That means the preview is a prediction of real behaviour, not a different algorithm.

## Safety

- Install refuses if `OpenAL32_real.dll` exists (interrupted install) and verifies the backup hash before replacing anything.
- Remove is two-step: the first press arms the button for five seconds, the second performs it.
- Uninstall refuses to delete a proxy whose hash no longer matches the manifest, and reports what to do instead.
- Mod removal verifies the recorded hash before deleting.
- Nothing in the manager touches score submission, Steam APIs or integrity checks.

## Verification status

- Core services: 15/15 headless scenarios (`tools/test_manager.sh`).
- UI: launched against a demo installation under Xvfb and captured in `docs/evidence/manager-ui.png`. The UI has not been exercised interactively by a human; the screenshot confirms it renders real data end to end.
- Not yet implemented: mod profiles, asset/config browsing, remote catalogue. See `docs/ROADMAP.md`.
