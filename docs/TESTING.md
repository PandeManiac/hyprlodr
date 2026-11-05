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

## Pre-entrypoint fixture (Windows x64 under Wine)

`tools/build_windows.sh` creates a proxy-like DLL with the production
`callsite_hook.c`, `record_route_hook.c`, and `record_route.c` linked into its
`DllMain`. A statically importing client contains an assembly-authored
`CALL rel32` and verifies that `reserved != NULL` leads to dormant installation
before its executable entry function runs. It checks unchanged forwarding,
manifest-style route publication and activation, tag-2/unrelated pass-through,
logical disable without code rewrite, no early opt-in, wrong expected bytes,
failed route publication, and a separate late `LoadLibrary` rejection.

The fixture proves behavior for its generated executable and Wine's startup
loader path. It does not substitute for the pinned game executable's runtime
anchor/import/TLS checks, which remain fail-closed in the proxy.

## First `_Fiopen` archive-open ordering (Windows x64 under Wine)

The pinned executable imports the decorated
`?_Fiopen@std@@YAPEAU_iobuf@@PEBDHH@Z` symbol from `MSVCP140.dll`; the import
slot being patched belongs to the executable, not to MSVCP140. The compiled
fixture links a client with that exact decorated import from a stand-in
`msvcp140.dll`, loads the actual `file_hook.c`, and counts calls at the original
target. It covers a held first archive open until redirect publication,
concurrent opens, unrelated paths, bootstrap failure, worker-creation failure,
replacement-disabled pass-through, and timeout fallback. The timeout case also
checks a later archive open remains on the original path after bootstrap
finishes, preventing an appended archive from being paired with a disabled
record route. The callsite fixture also enables its wrapper while the gate is in
the intermediate ACTIVATING state and verifies pass-through until READY. These
fixtures prove the synthetic import, timeout fallback and route gating under
Wine; they do not prove every game resource lookup follows the archive-open
path or that the first cold `hand2` lookup occurs after activation.

## Managed callsite fixture (Windows x64 under Wine)

`tools/build_windows.sh` builds `tests/callsite_hook_client.exe` from the same
`callsite_hook.c` and `record_route_hook.c` sources linked into the proxy.
`tools/test_windows.sh` always executes it, even when no original OpenAL DLL is
provided. The fixture emits executable Windows x64 machine code with a known
five-byte `CALL rel32`, first calls the original target, then runs a
startup-order case where the only resource thread parks at an archive-open gate
before the callsite. The fixture
installs and enables while that thread is blocked, releases it, and verifies the
patched request and restored descriptor.

The fixture checks direct and relayed replacement targets, the four argument
registers and return value, the tag-1 enlarged record route (239 vertices,
1,191 indices, 22,056-byte replacement extent), tag-2 and unrelated pass-through,
MSVC short and long string layouts, request metadata restoration and the
parsed-object pointer, plus unchanged source-record metadata. It also exercises
repeated install, quiescence rejection, expected-byte/original-target mismatch,
relay allocation failure, page-protection failure, instruction-cache-flush
rollback, nested invocation, concurrent independent requests and enable-state
toggling, and disablement while a call remains in flight. It never unpatches or
frees the relay; that is the tested lifetime contract.

This fixture demonstrates execution of the compiled hook implementation under
Wine. Its quiescent installation is established by construction in the fixture;
it does not prove the game has an equivalent safe install phase.

The compiled `tests/route_manifest_client.exe` runs the native manifest reader
under Wine against `.tmp/archive-tests/asset-hand2-appended`. When the installed
game executable is present, the runner uses its real path and the manifest's
pinned hash; otherwise it substitutes the fixture executable hash only for
that test input. It covers a valid route, executable/archive hash mismatch,
changed ciphertext, wrong record/table/tag, truncation, missing fields,
unsupported schema, overflow and concurrent reads of process-lifetime route
state. The proxy regression run checks that `experimental_record_route` is off
by default. On the unsupported Wine stand-in an opt-in request is reported as
`executable_unsupported`. A valid manifest with no early process-start hook reports
`validated_early_hook_missing`. With the early environment opt-in, only the
static-import startup fixture reports a dormant installed hook; an unsupported
stand-in executable still fails fingerprint validation and cannot enable the
route. The real game has not been launched with this path.

## Loader scenarios (native, 41/41 passing)

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
| `native_abi_mismatch` | ABI 1 descriptor rejected by the current host |
| `state_dirs_are_per_mod` | Two mods write to their own directories and see different paths |
| `state_dir_absent_without_root` | No state root means `state_dir()` is NULL, and a mod relying on it fails cleanly |
| `state_dir_persists_across_runs` | A counter written to `state_dir()` reads back as 2 on a second host process |
| `descriptor_too_small` | Truncated `struct_size` rejected |
| `null_descriptor` | NULL handshake result rejected |
| `missing_entry_export` | Library without the entry symbol rejected |
| `corrupt_library` | Non-PE file reported with its error; valid mods still load |
| `empty_mod_directory` | Reported as an error, zero mods |
| `initialization_failure_stops_dependents` | Pre-failure mod shut down, dependent never started, rollback journal reports 2 undone changes |
| `rollback_leaves_no_residue` | No mod initializes after a failure |
| `state_remove_rolled_back_on_failure` | A failing mod deletes another mod's key and adds one of its own; after rollback the keeper observes the deleted value restored and the added key gone (asserted from its `shutdown()`, i.e. through the public ABI) |
| `file_redirect_registration` | Exact match, wrong file, case-insensitive match, relative path, non-boundary suffix and prefix path — six probes against the published registry |
| `file_redirect_rolled_back_with_failing_mod` | A redirect registered by a mod that then fails is not published |
| `image_access_reads_the_verified_image` | A mod reads 8 bytes at offset 16 of a 64-byte image supplied by the host and logs `0x1011121314151617` |
| `image_access_refused_without_image` | With no image, the read is refused, the mod logs it and the run still succeeds |
| `image_access_refuses_out_of_range` | An offset past the end is refused instead of reading past the buffer |
| `example_game_info_refuses_a_small_image` | The shipped `example.game-info` treats a refused read as normal and still initializes |
| `example_game_info_detects_an_unknown_build` | A readable image of the wrong bytes is rejected by the example's own hash comparison, without crashing |
| `image_write_patches_verified_bytes_and_is_restored` | A patch is visible through a second read, `image_restored` is logged, and `--image-out` proves the bytes are back after shutdown |
| `image_write_refused_without_image` | No image means no patch: the mod logs a refusal, no `image_write` event exists, and the run succeeds |
| `image_write_refuses_out_of_range` | A write past the end is refused and leaves the image unchanged |
| `image_write_rolled_back_with_failing_mod` | A mod that patches and then fails has its patch undone before the loader returns |
| `symbol_lookup_resolves_verified_bytes` | A published name resolves to its RVA and length (`probe` → `0x10`, 8 bytes) |
| `symbol_lookup_refuses_unknown_name` | A name the host does not publish is refused, and the mod keeps running |
| `symbol_lookup_refuses_tampered_bytes` | The name exists and the range is valid, but altered bytes make the lookup fail |

The strict warning set (`-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion`) applies to the loader, host, samples and fixtures.

## Package format (19/19 passing)

`tools/tests/test_package.py` exercises the real tooling against real binaries:

| Scenario | Asserts |
|---|---|
| `pack_and_validate_roundtrip` | Pack then inspect reproduces the manifest and entries |
| `rejects_traversal` / `rejects_absolute_path` / `rejects_backslash_path` | Unsafe names refused |
| `rejects_unknown_root_file` | Files outside `native/`, `assets/`, `config/` refused |
| `rejects_symlink_entry` | Non-regular entries refused |
| `rejects_duplicate_entry` | Duplicate archive members refused |
| `rejects_missing_entry_file` | Declared entry must exist |
| `rejects_invalid_id` / `rejects_invalid_dependency` / `rejects_bad_api` | Manifest field validation |
| `rejects_wrong_platform_suffix` | Entry extension must match platform |
| `rejects_zip_bomb_ratio` / `rejects_oversized_entry` | Size and compression limits enforced |
| `rejects_non_zip` | Garbage archives refused |
| `install_list_remove` | Install, idempotent re-install, index, removal |
| `install_refuses_different_content` | Same id with different bytes is refused |
| `descriptor_cross_check_detects_mismatch` | Manifest/descriptor version mismatch refused |
| `descriptor_cross_check_accepts_real_mod` | Real ABI descriptor accepts a correct manifest |

The cross-check tests call `hyprlodr-host --describe`, which loads the library and calls the descriptor query only — no `initialize`, so packaging never executes mod behaviour.

## Windows artifacts under Wine (verified)

- `hyprlodr-host.exe` loads the five sample **DLLs** in the same order and shuts them down in reverse.
- `proxy_client.exe` loads the replacement `OpenAL32.dll`, resolves forwarded `alGetError`, `alcOpenDevice` and `alGenSources`, and the bootstrap thread initializes all five mods inside that stand-in process, writing `hyprlodr-attach.json` and `hyprlodr-events.jsonl`.

This proves the Windows loader path, the forwarder table and the attach bootstrap. It does **not** prove HYPER DEMON runs with it.

The attach record also reports the file hook: the stand-in's modules import `CreateFileA` but no CRT file API, so the run asserts `CreateFileA=patched(n)` with a module count, `fopen=absent`, and that the stand-in's redirected open returned the mod's bytes, with the record written to `hyprlodr-files.jsonl`.

## Asset probe (`asset_probe_suite`)

`tools/re/asset_probe.py --selftest` builds a synthetic archive with the cipher the decompiled loader implies, encrypts it, decrypts it, and asserts the entry table parses back to the original names, offsets and sizes — including a corrupt-byte case. This is a model check, not a claim about the real container: the tool's real-file mode reports `RESULT: decryption does not yield names` and exits non-zero, which is the current, honest state. It also verifies that the binary's CRC table at RVA `0x36f700` equals the standard CRC-32 table (it does), which is what makes `crc32(name) & 0xffff` a confirmed per-entry key rather than a guess.

## Installer safety (3/3 passing)

`tools/tests/test_install.py` drives `tools/hyprl_install.py` against throwaway game directories:

| Scenario | Asserts |
|---|---|
| `install_preserves_existing_mod` | A mod file that already exists is recorded as a backup, the build's copy is installed, and uninstall restores the user's bytes |
| `failed_install_rolls_back` | An install that fails after the proxy was replaced restores the original library, removes the backup and the manifest, and leaves the blocking file intact |
| `uninstall_keeps_changed_mod` | A mod whose bytes changed since installation is kept (and reported), untouched mods are removed, original library restored |

## Title mask delivery (3/3 passing)

`tools/tests/test_install_mask.py` covers the glyph mask the replacement depends on. It must arrive by
installing rather than by hand-copying, and a mask the user already had must survive the round trip.

| Scenario | Asserts |
|---|---|
| `mask_is_delivered` | `--title-mask` is copied to `hyprlodr-title-mask.raw`, recorded in the manifest, and removed by uninstall |
| `existing_mask_is_restored` | A mask already in the game directory is backed up, replaced, and put back by uninstall |
| `missing_mask_is_reported` | A `--title-mask` path that does not exist fails before the proxy is touched: no backup library, no manifest |

## Package payload files (5/5 passing)

`tools/tests/test_package_files.py` covers the `files` manifest key that lets a package ship data the
game itself opens - the title mask. Everything runs in throwaway directories.

| Scenario | Asserts |
|---|---|
| `payload_installs_beside_the_game` | The mask lands next to the game, is recorded in the index, and is removed with the mod |
| `reinstall_restores_a_deleted_payload` | Re-installing a package puts back a payload file that disappeared |
| `changed_payload_is_kept` | A mask the user changed is kept on removal, and the removal says so |
| `escaping_target_is_refused` | `..`, absolute, drive-letter, `mods/` and index-colliding targets are all refused at pack time |
| `source_outside_payload_prefix_is_refused` | A `files` source taken from `native/` is refused |

The mask itself is not committed: it is rendered by `tools/make_title_mask.py` from a face inside the
user's own installation, and `/assets/hyprlodr-title-mask.raw` is ignored by Git.

## Face recovery (5/5 passing)

`tools/tests/test_title_mask_from_game.py` covers `tools/title_mask_from_game.py`, which recovers the
typeface from the container instead of trusting a face copied out earlier. The tests build their own
encrypted container, so they never read the game executable or its assets.

| Scenario | Asserts |
|---|---|
| `recovers_face_names` | A hand-made sfnt at byte 0 of a slice is cracked, decrypted, and identified by its `name` table |
| `recovers_and_renders` | A real system font behind a 24-byte slice header survives the round trip with the header stripped, and the rendered mask is 853x480 with plausible ink |
| `ignores_non_font_records` | Noise is not reported as a face |
| `unknown_family_is_reported` | A family that is not there fails while naming what was actually found |
| `records_are_read_from_both_shapes` | Both log shapes (JSON lines, one keyed object) yield `(offset, size)` records |

## Manager core (36/36 passing)

`tools/test_manager.sh` builds and runs `manager/HYPRLODR.SelfTest`, which needs no display and is wired into ctest as `manager_suite` when the .NET SDK is present.

| Scenario | Asserts |
|---|---|
| `steam_discovery` | Synthetic Steam tree: library, appmanifest, build id, Proton prefix and version |
| `discovery_manual_path` | Manual folder override resolves an installation |
| `loader_install_and_inspect` | Backup hash, proxy placement, mods copied, manifest hashes match |
| `loader_refuses_double_install` | Interrupted/duplicate install is refused |
| `loader_uninstall_restores_original` | Original bytes restored, manifest and evidence removed, empty mods dir cleaned up |
| `loader_refuses_changed_proxy` | Tampered proxy blocks removal and the original is kept |
| `catalog_reads_registry_and_disabled` | Registry metadata and `disabled.txt` applied |
| `catalog_order_matches_loader` | Manager order equals `z.foundation, a.palette, b.audio, m.overlay, 00.final` |
| `catalog_detects_missing_dependency` | Reported with status `dependency missing` |
| `catalog_detects_cycle` | `cycle among` reported |
| `catalog_detects_version_mismatch` | `@>=2.0.0` against 1.0.0 refused |
| `catalog_blocked_dependency_is_not_a_cycle` | A disabled dependency is reported as blocked, never as a cycle |
| `version_constraints` | `^`, `~`, `>=`, `>` semantics and spec splitting |
| `event_log_reader` | Parses JSONL and tolerates a truncated final line |
| `package_validation` | Valid package accepted; traversal and disallowed files refused |
| `package_import_and_registry` | Import writes the binary and a registry entry the catalog can read |
| `profiles_round_trip` | Capture, save and reload a profile; name normalised, enabled set preserved |
| `profiles_apply_rewrites_disabled_list` | Applying a profile flips mod states, rewrites `disabled.txt`, and reports removed ids |
| `profiles_tolerate_corrupt_file` | Corrupt JSON loads as empty; blank names dropped; long names clamped |
| `run_report_reads_evidence` | Attach record and event log parsed into stage, pid, fingerprint counts, init count and last error |
| `run_report_tolerates_absence` | Missing files report no run; a corrupt attach record still yields the parsed events |
| `loader_preserves_existing_mod` | Existing mod file backed up and restored by uninstall |
| `loader_install_rolls_back` | Failed install restores the proxy, removes the backup/manifest, leaves the blocking file intact |
| `loader_uninstall_keeps_changed_mod` | A modified mod is kept; untouched mods and the loader are removed |
| `file_log_reads_opens` | Parses `hyprlodr-files.jsonl`: two opens over two paths, one redirect, first at 35 ms, and a different event kind in the same file is not counted as an open |
| `file_log_tolerates_garbage_and_caps` | A corrupt line does not change the counts, only the newest 200 records are kept, and the summary says so |
| `file_log_absent` | A missing file log reports no opens instead of failing |
| `file_log_lists_recent_opens` | Newest-first lines carry time, API, path, the redirect target and a `(failed)` marker |

UI verification: the app was launched under Xvfb against `build-win/demo-game` (fake install with five mods, one disabled) and captured to `docs/evidence/manager-ui.png`. That confirms rendering and data binding; it is not an interactive user test.

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

## Build fingerprints (verified in the real game, 2026-10-31)

Before any address-dependent work, HYPRLODR proves the running executable is the build that was analysed. `tools/re/analyze_pe.py` locates short constants and the RIP-relative instructions that reference them, then records an RVA plus the FNV-1a 64 hash of the bytes that belong there. Only hashes are stored: neither the manifest nor `native/win32/game_anchors.h` contains game content.

Anchors for the shipped build (`docs/evidence/2026-10-31-anchors.json`): four string constants and four reference sites.

| Check | Result |
| --- | --- |
| `anchor_test` (ctest) | FNV-1a reference vectors, out-of-range anchors, single-byte corruption, NULL arguments — passing |
| `analyze_pe.py --verify` against the shipped binary | 8/8 anchors verified |
| Wine stand-in host (`proxy_client.exe`) | 0/8 anchors, `build_matches=false` — an unknown build is rejected, not assumed |
| Real game (`hyperdemon.exe` under Proton) | `module_base=0x140000000`, timestamp `0x66ecd8b5` matches, **8/8 anchors matched**, `build_matches=true` |
| Live cipher tables (real game) | `tables_dumped=true`; the 512 bytes copied out of the running image are byte-identical to the file's `.rdata`, so no runtime transformation occurs |
| Live cipher tables (Wine stand-in) | `tables_dumped=false` — the RVAs are outside that image, so the proxy declines instead of reading past it |
| Crash filter (real game) | No `hyprlodr-crash.json` was written during the 2026-10-31 run, so the early exit raises no unhandled exception — it is not an in-process fault |
| Crash filter (Wine stand-in) | A clean `proxy_client.exe` exit writes no crash record; asserting this proves the chained filter stays silent unless an exception is genuinely unhandled |
| Start-up delay (Wine stand-in) | Absent config reports the built-in `delay_ms: 2500`; `hyprlodr-config.json` with `delay_ms: 0` is honoured and all five mods still initialize |
| In-game initialization (real game, `delay_ms: 0`) | 35 events, **5/5 `initialize_ok`** in dependency order (`z.foundation, a.palette, b.audio, m.overlay, 00.final`), zero failures, and `state_dir` resolved inside `<game>\hyprlodr-state\00.final` |

`docs/evidence/2026-10-31-fingerprint.json` is the attach record written by the game process itself. It also shows `module_size=4214784`, matching the PE `SizeOfImage`, which is the range the anchors were verified against.

Re-running the fingerprint check:

```bash
GAME=/path/to/steamapps/common/hyperdemon
python3 tools/re/analyze_pe.py "$GAME/hyperdemon.exe" --verify native/win32/game_anchors.h
# inside the game, after installing the loader:
cat "$GAME/hyprlodr-attach.json"   # expect build_matches=true, anchors_matched == anchors_total
```

A mismatch is reported, never optimised away. `build_matches=false` still loads
ordinary mods, but the experimental callsite hook fails closed unless its
process-start anchors and call bytes match; route activation additionally
requires the full manifest preflight and archive redirect match.

Note on the 2026-10-31 re-runs: the game process exits a few seconds after start-up in this session (it also exits with the stock `OpenAL32.dll`, so this is environmental, not caused by the proxy), and it writes **no** `hyprlodr-crash.json`, so the exit is not an unhandled exception inside the game process. With the default 2500 ms delay that exit arrives first and no mod ever starts; the attach record then still reads `stage: "initializing"` or `"attached"`. Setting `delay_ms: 0` closes the gap: `docs/evidence/2026-10-31-initializing.json` records 35 events and all five mods initialized, in order, inside `hyperdemon.exe` with `build_matches=true`. Shutdown events are absent by design — process termination runs no mod code.

## File hook in the real game (2026-11-01)

Five runs with the loader installed in the real installation, `delay_ms: 0`, game terminated after 10–35 s, original library restored each time (`396f8873...420b4`):

| Run | Hook | Result |
|---|---|---|
| 1 | `CreateFileA` only | `file_hook: "installed"`, `file_opens_logged: 0`, 5/5 mods initialized |
| 2 | `CreateFileA` + `fopen` + `fopen_s` | `file_hook_detail: "CreateFileA=patched fopen=patched fopen_s=patched"`, `file_opens_logged: 0` |
| 3 | same, 30 s of game time | `file_opens_logged: 0` |
| 4 | `+ _wfopen`, main module | `CreateFileA=patched fopen=patched fopen_s=patched _wfopen=patched`, `file_opens_logged: 0` |
| 5 | all four targets in **every loaded module** | `CreateFileA=patched(8) fopen=patched(2) fopen_s=patched(2) _wfopen=patched(2) modules=39`, `file_opens_logged: 1` |

Run 5 proves the shim is live inside the game process: it captured a real `CreateFileA` open of `\\.\pipe\wine_plugplay` from a patched module (the file log records `t_ms`, `thread`, `via`, `redirected` and `ok`, so it reads as a timeline). A 60 s run with every target patched in 39 modules stayed alive the whole time (checked every 5 s), initialized 5/5 mods, and still recorded exactly one open — at `t_ms: 35`, the same pipe. So the shim was live from ~35 ms after install and the game made no file open through any patched slot in a full minute. No asset open appeared in 22–35 s of game time, so HYPER DEMON's asset reads do not use any patched import slot. Measured imports of `hyperdemon.exe`: `KERNEL32.dll` exposes only `CreateFileA`/`ReadFile` as file APIs, while `api-ms-win-crt-stdio-l1-1-0.dll` exposes `fopen`, `fopen_s` and `_wfopen`. With all of them patched across 39 modules, the remaining candidates are the kernelbase/`NtCreateFile` path (no import slot to patch) or the Steam API file layer. No in-game asset replacement has been achieved. Evidence: `docs/evidence/2026-11-01-file-hook.json`.

## Still unverified

- **The game's asset path**: no asset open was observed in-game even with `CreateFileA`, `fopen`, `fopen_s` and `_wfopen` patched in 39 modules; the reads go through something without an import slot (kernelbase/`NtCreateFile`) or the Steam API file layer.
- **In-game asset replacement**: file redirection is proven only against the stand-in host.
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
- The package validator/packer and the desktop manager now exist, with their own suites (`package_suite`, `manager_suite`); they are no longer unverified areas.

## In-game file I/O (2026-11-01)

Reading `/proc/<pid>/fd` during a live session showed `asset/asset` held open by the game while the file hook logged nothing. The import table explains it: the game imports `MSVCP140!_Fiopen`, the C++ stream open, whose call into the UCRT never passes an import slot. After intercepting `_Fiopen` (and installing the hook from `DllMain` so the ~200 ms start-up window is covered), a plain run logged 14 opens:

| t_ms | via | path |
|---|---|---|
| 4 | `_Fiopen` | `steamuser.log` |
| 39 | `CreateFileA` | `\\.\pipe\wine_plugplay` |
| 215 | `_Fiopen` | `asset/asset` |
| 1257 | `_Fiopen` | `…/HyperDemon/training` (absent) |
| 1266–1274 | `_Fiopen` | `…/HyperDemon/config`, `cache/scores`, `cache/names`, `user` |

With a mod registering `asset/asset` → its own file, the same open came back `"redirected":true,"opened":"…\\hyprlodr-state\\r.asset\\asset.bin"`, 7/7 mods initialized and the game ran normally (the replacement was a hardlink, so the bytes were identical). Hook detail for that run: `CreateFileA=patched(7) fopen=patched(1) fopen_s=patched(1) _wfopen=patched(1) _Fiopen=patched(1) _wfopen_s=absent modules=39`.
