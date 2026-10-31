# Roadmap

## Done

**Milestone 0 — reconnaissance.** Local game installation located and hashed; executable and dependency PE metadata parsed; Proton installation confirmed; REA/Ghidra availability checked; build tooling verified on both targets.

**Milestone 1 — native loader (Level 1).** ABI 2 with append-only descriptor extension; deterministic resolution with lexicographic tie-breaks; semantic-version constraints; conflicts; disabled mods; journaled rollback on initialization failure; reverse shutdown; JSON event log; five independently compiled sample mods; 23-scenario suite passing on Linux.

**Milestone 2 — real game integration (Level 2, verified 2026-10-31).** HYPER DEMON launched through Steam/Proton with the proxy installed; the real `hyperdemon.exe` process initialized all five mods in dependency order and wrote attach/event evidence from inside the process. Uninstall restored the original `OpenAL32.dll` byte-for-byte. See `docs/TESTING.md` and `docs/evidence/`.

**Windows transport (part of Level 2).** Windows x64 build with no SDK/mingw/CRT dependency; 175-forwarder OpenAL32 proxy; loader-lock-safe bootstrap thread; five mod DLLs loading in order under Wine; forwarder resolution and attach evidence verified in a stand-in process.

**Tooling.** Read-only diagnostics (Steam discovery, multi-library, PE parsing, Proton detection), transactional installer with hash manifest and rollback, proxy `.def` generator, Windows build and test drivers.

## Next, in order

1. **Package format + validator.** ZIP container with `manifest.json`; defensive extraction (no traversal, no absolute paths, no symlinks); manifest/descriptor cross-check; packer script so a developer can build a mod with one command.
2. **Desktop manager (Milestone 3).** Avalonia application: game discovery, loader install/remove through the same manifest logic, mod list with enable/disable, dependency view, log viewer, rollback.
4. **Asset reconnaissance (Milestone 5 groundwork).** Analyse `asset/asset` with Ghidra/REA: container header, table layout, resource ids, then the lookup function. No replacement code before the format is documented as observed.
5. **Engine-facing API.** Only after 4, and only at a verified boundary (resource lookup or a narrowly fingerprinted hook with an explicit compatibility gate).

## Explicitly deferred

- Remote catalogue, accounts, publishing, moderation.
- Anything touching score submission, DRM or integrity checks.
- Live plugin unload while the process runs.
- Address-based hooks without fingerprints and a safe-failure path.

## Known gaps

- The real-game check still only exercises start-up; audio quality and gameplay behaviour were not observed.
- No CI pipeline is configured; the build and test scripts are ready to be wired into one.
- No sanitizer build.
- No package tooling, so the package-format section of the brief is unimplemented.
- Windows tests depend on Wine being installed on the test machine.
