# Testing

Passing a synthetic-host test is **not** the same as passing a real-game test. The two are reported separately below.

## Running

```bash
# Linux native loader suite (23 scenarios)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure

# Windows x64 artifacts under Wine
tools/build_windows.sh build-win "/path/to/HYPER DEMON/OpenAL32.dll"
tools/test_windows.sh  build-win "/path/to/HYPER DEMON/OpenAL32.dll"

# Read-only installation diagnostics
python3 tools/hyprl_diagnose.py --json report.json
```

Every scenario copies real, independently compiled shared libraries into a temporary directory and runs `hyprlodr-host`. Assertions read the JSON event log the loader itself wrote — no callback simulation, no fabricated orderings.

## Loader scenarios (native, 23/23 passing)

| Scenario | Asserts |
|---|---|
| `five_mod_lifecycle` | Init order `z.foundation, a.palette, b.audio, m.overlay, 00.final`; reverse shutdown; shared state and capabilities observed by `00.final` |
| `deterministic_startup` | Identical order for shuffle seeds 0–11 |
| `repeated_host_execution` | 10 consecutive runs agree |
| `lexicographic_tie_break` | Independent mods order by id, not by discovery |
| `diamond_dependency` | `d.bottom` first, `d.top` last, stable across seeds |
| `version_constraint_satisfied` | `^1.0.0` accepted against 1.0.0 |
| `state_passed_between_mods` | Writer state visible to its dependent |
| `missing_dependency` | `missing dependency: not.there`, nothing initializes |
| `circular_dependency` | `cycle among`, nothing initializes |
| `self_dependency` | Self-cycle detected |
| `disabled_dependency` | `dependency is disabled`, dependent never starts |
| `disabled_file_list` | `disabled.txt` comments/blank lines parsed, disabled mod skipped |
| `duplicate_mod_id` | Reported; exactly one copy initializes |
| `version_incompatible` | `does not satisfy`, incompatible mod never starts |
| `conflicting_mods` | Declared conflict reported |
| `native_abi_mismatch` | ABI 1 descriptor rejected by an ABI 2 host |
| `descriptor_too_small` | Truncated `struct_size` rejected |
| `null_descriptor` | NULL handshake result rejected |
| `missing_entry_export` | Library without the entry symbol rejected |
| `corrupt_library` | Non-PE file reported with its error; valid mods still load |
| `empty_mod_directory` | Reported as an error, zero mods |
| `initialization_failure_stops_dependents` | Pre-failure mod shut down, dependent never started, rollback journal reports 2 undone changes |
| `rollback_leaves_no_residue` | No mod initializes after a failure |

The strict warning set (`-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion`) applies to the loader, host, samples and fixtures.

## Windows artifacts under Wine (verified)

- `hyprlodr-host.exe` loads the five sample **DLLs** in the same order and shuts them down in reverse.
- `proxy_client.exe` loads the replacement `OpenAL32.dll`, resolves forwarded `alGetError`, `alcOpenDevice` and `alGenSources`, and the bootstrap thread initializes all five mods inside that stand-in process, writing `hyprlodr-attach.json` and `hyprlodr-events.jsonl`.

This proves the Windows loader path, the forwarder table and the attach bootstrap. It does **not** prove HYPER DEMON runs with it.

## Real game (verified 2026-10-31)

HYPER DEMON was launched through Steam (Proton Experimental, Steam Runtime 4) with the loader installed. Recorded evidence, committed verbatim as `docs/evidence/`:

`hyprlodr-attach.json`:

```json
{"stage":"mods_loaded","pid":316,"proxy_dir":"S:\\steamapps\\common\\hyperdemon","exe":"S:\\steamapps\\common\\hyperdemon\\hyperdemon.exe","loader_abi":2,"status":0,"status_text":"ok"}
```

`hyprlodr-events.jsonl` (30 events) contains the five `discovered` events from `S:\steamapps\common\hyperdemon\mods\*.dll`, the five `initialize_ok` events in dependency order, and the mods' own log lines — for example `z.foundation: "created shared test state and registered test.foundation"` and `00.final: "confirmed all prerequisites and shared state"`. Those strings can only be produced by the mod DLLs running inside the process that loaded them.

Verified outcomes:

- The replacement `OpenAL32.dll` is accepted by the Windows loader inside Proton: `hyperdemon.exe` has 26 static OpenAL imports, so the process cannot start at all unless every forwarded export resolves.
- All five mods initialized in order `z.foundation, a.palette, b.audio, m.overlay, 00.final`; the game ran for 29 s without error.
- `hyprl_install.py uninstall` restored `OpenAL32.dll` to `396f88738ce20905e933cb7d8c696351e41bd7e72e80b8cfdadf20ab613420b4`, byte-identical to the shipped file.

## Still unverified

- **Audio output quality**: the process proves the `al*`/`alc*` symbols resolve into the real library; nobody has listened to the game with the proxy installed.
- **Game-level behaviour** beyond start-up: no gameplay, scoring or online feature was exercised.
- **Windows native** (non-Proton) execution.
- Any claim about competitive-scoring safety.
- A `/proc/<pid>/maps` check for `OpenAL32_real.dll` was inconclusive: it targeted the Proton launcher pid rather than the game's Wine process. Symbol resolution is the stronger evidence anyway.

Re-running the real-game check:

```bash
GAME=/path/to/steamapps/common/hyperdemon
python3 tools/hyprl_install.py install --game-dir "$GAME" --build build-win --yes
steam -applaunch 1743850          # wait ~20 s
cat "$GAME/hyprlodr-attach.json"  # expect stage=mods_loaded, exe=...\hyperdemon.exe
python3 tools/hyprl_install.py uninstall --game-dir "$GAME" --yes
sha256sum "$GAME/OpenAL32.dll"    # expect 396f8873...420b4
```

## Limitations

- No CI runs the Windows build automatically; `tools/build_windows.sh` and `tools/test_windows.sh` are the entry points a runner should call.
- Sanitizers are not wired into the Linux build yet.
- The package validator/packer and the desktop manager do not exist yet, so no package-format tests are claimed.
