# Session progress

## 2026-10-31 (local session)

**Recovery.** Discovered the local checkout held the cloud agent's uncommitted work (no commits at all) and that `~/hyprlodr-recovery/hyprlodr-hJcCSK03` (commit `4c0bd6b`) differed from it only by the two GCC fixes (`dlsym` function-pointer cast, misleading indentation) plus `.gitignore`. Adopted those fixes verbatim, built, ran the existing lifecycle test, and committed the baseline as `f9a8a14`. No recovery data was modified or deleted.

**Loader (ABI 2).** Replaced the single-file MVP loader with the documented design: separate resolve/init phases, append-only descriptor extension, semantic-version constraints, conflicts, disabled mods, deterministic lexicographic ordering, seeded discovery shuffling, journaled rollback, `state_remove`/`is_loaded` host services, and sequential event numbers.

**Tests.** Replaced the single lifecycle script with a 23-scenario suite that runs the real host binary and asserts on the loader's own JSON event log. All 23 pass, including ABI mismatch, duplicate ids, cycles, diamond dependencies, rollback and disabled-dependency cases.

**Windows x64.** Added a self-contained target layer (minimal libc on kernel32, generated import library, `GetCommandLineA` entry shim) so the loader builds without any SDK, mingw or CRT. Built the five sample mod DLLs and verified them loading in order under Wine. Built the OpenAL32 proxy with 175 generated PE forwarders and verified forwarding plus the attach bootstrap in a stand-in process.

**Tooling.** Read-only diagnostics with an independent PE parser (verified against the real installation), transactional installer with hash manifest and rollback, proxy `.def` generator, Windows build and test drivers, full documentation set.

**Mod profiles (manager).** `ProfileStore` captures the enabled set by name into `<game>/hyprlodr-profiles.json` and re-applies it by rewriting `disabled.txt`, so the native loader and the UI agree. Removing a mod does not break a profile: applying it reports the ids that are no longer installed. Three headless scenarios cover round-trip, apply-with-rewrite and tolerance of a corrupt profile file.

**Mod persistence (ABI 3).** `host_api.state_dir()` gives each mod a writable directory, reserved before `initialize()` and rooted at `<game>/hyprlodr-state` in the game. Two loader scenarios cover per-mod isolation and the no-root failure; the Windows host (ABI 3) and the Wine proxy path both pass. Uninstall leaves the directory alone: it is user data.

**Asset container triage.** `asset/asset` is 306 MB at 8.00 bits/byte with no strings and no zlib/bzip2/xz framing. Decompilation shows a C++ stream that opens the literal path and resolves entries after an internal header (`FUN_140212490` → `FUN_140110570`), with `FUN_140213cb0` building `"Can't find asset "`. Asset replacement stays blocked on understanding `FUN_140212c90`.

**Build fingerprints (Level 4 groundwork, verified 2026-10-31).** `tools/re/analyze_pe.py` parses the PE, locates four short constants and the four RIP-relative instructions that reference them, and writes `native/win32/game_anchors.h`: RVA + FNV-1a 64 hashes only, no game content. `native/src/anchors.c` verifies them against a mapped image; the proxy parses the main module headers and reports `module_base`, `module_size`, `build_timestamp`, `build_matches` and per-anchor counts in `hyprlodr-attach.json`. Verified in the real game: 8/8 anchors matched at base `0x140000000`, timestamp `0x66ecd8b5`. Verified negatively under Wine: 0/8 and `build_matches=false` for a stand-in host, while the five mods still load. `anchor_test` covers the verifier in ctest.

**Real game (Level 2 verified).** Installed the loader into the local Steam installation, launched HYPER DEMON through Steam/Proton, and confirmed from inside the process that all five mods initialized in dependency order (`hyprlodr-attach.json` named `hyperdemon.exe`, 30 lifecycle events, five mod-authored log lines). Uninstalled afterwards and verified `OpenAL32.dll` is byte-identical to the shipped file. Evidence committed in `docs/evidence/`.

**Mod packaging (schema 1).** ZIP + `manifest.json` with defensive validation (traversal, absolute paths, backslashes, symlinks, duplicates, size and compression limits), an install registry for the future manager, and manifest/descriptor cross-checking through a new `hyprlodr-host --describe` mode that reads ABI descriptors without initializing mods. 19 package scenarios pass alongside the 23 loader scenarios.

**Desktop manager (Milestone 3).** Avalonia application over a tested core: Steam discovery (multiple libraries, manual override, Proton detection), loader install/remove with hash manifests and rollback, mod catalog with enable/disable and removal, package import, the exact load order the native loader will use, validation problems, and the event log viewer. Core services covered by 16/16 headless scenarios; the UI was launched under Xvfb against a demo installation and captured in `docs/evidence/manager-ui*.png`. The screenshots exposed a real planner bug: a mod blocked by a disabled dependency was also labelled a cycle. Fixed with a regression test.

**Not done.** No game-facing capability yet (needs Ghidra/REA work on the executable and asset pack), no mod profiles or asset browsing, no audio-quality or gameplay verification, no remote catalogue.

Details: `docs/TESTING.md` (what is and is not verified), `docs/ROADMAP.md` (next steps).
