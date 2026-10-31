# Compatibility

## Tested game build

| Field | Value |
|---|---|
| Steam app | 1743850 |
| `buildid` | 15765018 |
| `hyperdemon.exe` SHA-256 | `7612dd72076002b7c0cb5cab8f7340a399176ecedefe9d3b4d0353fbdac93611` |
| `OpenAL32.dll` SHA-256 | `396f88738ce20905e933cb7d8c696351e41bd7e72e80b8cfdadf20ab613420b4` |
| Executable | PE32+ x86-64, GUI subsystem, timestamp 2024-09-19 |
| Runtime | Linux + Proton Experimental, exercised in the real game 2026-10-31; Windows native path untested |

`python3 tools/hyprl_diagnose.py` reproduces this table for any installation.

## Compatibility policy

HYPRLODR currently depends on **one** property of the game: that it statically imports `OpenAL32.dll` and imports its functions by name, not by ordinal. That is stable across game versions unless the developers change their audio dependency.

What would break the loader:

- The game switching to a statically linked OpenAL or a differently named library — the proxy would no longer be loaded.
- Changing to ordinal-only imports from `OpenAL32.dll` — the forwarder table would need explicit `@ordinal` entries; regenerate with `tools/gen_proxy_def.py` after inspecting the new import table.
- Bundling a differently named OpenAL (e.g. `openal32.dll` case changes on a case-sensitive filesystem) — the file name is chosen by the executable's import table.

What would **not** break it: game updates that change executable code, add assets, or change audio calls. No addresses are used anywhere.

## Mod ABI compatibility

| Host ABI | Mod ABI | Result |
|---|---|---|
| 2 | 2 | Supported |
| 2 | 1 | Rejected: `native ABI mismatch` |
| 2 | 2 with truncated `struct_size` | Rejected: `struct_size too small` |
| 2 | 2 with a larger `struct_size` | Accepted; unknown trailing fields are never read |

Descriptor fields may only be appended. The loader gates every newer field on `struct_size`, so adding fields later does not require an ABI bump for existing mods — only removing or reordering fields does.

## Known limitations

- Do not claim leaderboard safety for modded sessions; see `docs/SECURITY.md`.
- The installer matches file hashes, not game versions: after a Steam update that replaces `OpenAL32.dll`, run `uninstall` (or verify integrity) before reinstalling.
- Only x86-64 is supported; there is no 32-bit or ARM build.
- Running through Proton and running on Windows both load the same DLL, but only the Proton/Wine path has been exercised in this session.
