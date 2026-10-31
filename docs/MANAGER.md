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

The UI is deliberately thin. Everything testable lives in `manager/HYPRLODR.Core` and is exercised headlessly by `manager/HYPRLODR.SelfTest` (16 scenarios, wired into ctest as `manager_suite`):

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

- Mod profiles: capture the current enabled set under a name, then re-apply it later. Applying rewrites `disabled.txt` so the native loader agrees with the UI, and any profile id that is no longer installed is reported rather than silently ignored. Profiles live in `<game>/hyprlodr-profiles.json`.
- Last-run report: the Game tab summarises `hyprlodr-attach.json` and the event log — stage, pid, whether the build fingerprint matched (with the anchor count and module base), how many mods initialized, and the last error. A corrupt or truncated attach record still shows the log instead of failing.
- Core services: 21/21 headless scenarios (`tools/test_manager.sh`).
- UI: launched against a demo installation under Xvfb and captured in `docs/evidence/manager-ui.png`. The UI has not been exercised interactively by a human; the screenshot confirms it renders real data end to end.
- Not yet implemented: mod profiles, asset/config browsing, remote catalogue. See `docs/ROADMAP.md`.

## Mod start-up delay

The Game tab has a **Mod start-up delay (ms)** field next to the last-run panel. It reads and writes `<game>/hyprlodr-config.json`, the file the proxy reads on attach:

```json
{
  "delay_ms": 2500
}
```

The proxy waits that long after attaching before it resolves mods. If the game's process lives only a couple of seconds in a given setup, the default delay can outlive it and no mod event is ever written — the panel then shows `stage: "attached"` or `"initializing"` with zero events, which looks like a broken loader. Setting `0` initializes mods immediately.

The manager clamps the value to 0–60000, keeps any other keys already in the file, reports a malformed file instead of hiding it (the proxy silently falls back to 2500 ms in that case), and never creates the file until you press **Apply delay**.

## Run diagnosis

The Game tab shows one sentence above the last-run panel, derived from the attach record, the event log, the install state and the delay setting:

| Situation | What it says |
|---|---|
| Not installed | "The loader is not installed. Install it, then launch the game once." |
| Installed, never launched | "No run is recorded yet." |
| Process ended inside the start-up delay | "The game process ended during the 2500 ms start-up delay, so no mod initialized. Set the delay to 0 and launch the game again." |
| Process ended with no delay | Says the process ended anyway and points at the fingerprint and installed mods — it does **not** blame the delay. |
| Mods initialized | "5 mod(s) initialized in the last run (35 event(s) recorded)." |
| Mods initialized on a different build | Warning quoting the anchor count; address-dependent features must stay off. |
| Loader ran, nothing enabled | "initialized no mods: none are enabled… or all of them failed to load." |
| Loader reported an error | Error severity, quoting the loader's own message. |

`RunDiagnosis` is a pure function over `RunReport` + `LoaderConfig` + the install flag, so all of it is covered by headless scenarios rather than needing a running game.
