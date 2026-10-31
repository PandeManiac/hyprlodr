# Roadmap

## Done

**Milestone 0 — reconnaissance.** Local game installation located and hashed; executable and dependency PE metadata parsed; Proton installation confirmed; REA/Ghidra availability checked; build tooling verified on both targets.

**Milestone 1 — native loader (Level 1).** ABI 2 with append-only descriptor extension; deterministic resolution with lexicographic tie-breaks; semantic-version constraints; conflicts; disabled mods; journaled rollback on initialization failure; reverse shutdown; JSON event log; five independently compiled sample mods; 23-scenario suite passing on Linux.

**Milestone 2 — real game integration (Level 2, verified 2026-10-31).** HYPER DEMON launched through Steam/Proton with the proxy installed; the real `hyperdemon.exe` process initialized all five mods in dependency order and wrote attach/event evidence from inside the process. Uninstall restored the original `OpenAL32.dll` byte-for-byte. See `docs/TESTING.md` and `docs/evidence/`.

**Windows transport (part of Level 2).** Windows x64 build with no SDK/mingw/CRT dependency; 175-forwarder OpenAL32 proxy; loader-lock-safe bootstrap thread; five mod DLLs loading in order under Wine; forwarder resolution and attach evidence verified in a stand-in process.

**Mod packaging (schema 1).** ZIP container with a validated `manifest.json`, defensive extraction guards (traversal, absolutes, backslashes, symlinks, duplicates, size/compression limits), a publishable install registry, and manifest-to-descriptor cross-checking via `hyprlodr-host --describe` so packaging never executes mod behaviour. Pack/validate/install/list/remove CLI with a 19-scenario suite.

**Desktop manager (Milestone 3).** Avalonia application with Steam discovery (multiple libraries, manual override, Proton detection), loader install/remove with hash manifests and rollback, mod list with enable/disable, package import, exact load-order preview, validation problems and a log viewer. Core services covered by 16/16 headless scenarios; UI rendered against a demo installation and captured as evidence.

**Tooling.** Read-only diagnostics (Steam discovery, multi-library, PE parsing, Proton detection), transactional installer with hash manifest and rollback, proxy `.def` generator, Windows build and test drivers.

## Next, in order

1. **First game-facing capability.** Use Ghidra/REA on `hyperdemon.exe` and `asset/asset` to establish one narrow, evidence-backed extension point that a mod can actually change, with a compatibility gate and a documented failure mode.
2. **Mod profiles and asset management in the manager.**
5. **Engine-facing API.** Only after 4, and only at a verified boundary (resource lookup or a narrowly fingerprinted hook with an explicit compatibility gate).

## Explicitly deferred

- Remote catalogue, accounts, publishing, moderation.
- Anything touching score submission, DRM or integrity checks.
- Live plugin unload while the process runs.
- Address-based hooks without fingerprints and a safe-failure path.

## Known gaps

- The real-game check still only exercises start-up; audio quality and gameplay behaviour were not observed.
- The manager UI has been rendered headlessly and screenshotted, but not driven interactively by a person.
- Mod profiles and asset/config browsing are not implemented.
- No CI pipeline is configured; the build and test scripts are ready to be wired into one.
- No sanitizer build.
- Packages hold assets and config that the loader does not yet consume; only the native binary is installed today.
- Windows tests depend on Wine being installed on the test machine.
