# Security

## Native mods are trusted code

A HYPRLODR mod is a native shared library loaded into the game process. It runs with the game's full privileges: file system access, network access, memory, and the ability to crash or take over the process. **There is no sandbox and none is claimed.** "Validate the manifest" protects against mistakes and bad packaging, not against a malicious author.

Practical consequences:

- Only install mods whose source or author you trust.
- A mod authored for ABI 2 cannot corrupt the loader's memory by declaring the wrong size — `struct_size` is checked — but it can still do anything at runtime.
- Running the game with mods is not a security boundary for your Steam account or your machine.

## Installation safety

`tools/hyprl_install.py`:

- copies the shipped `OpenAL32.dll` to `OpenAL32_real.dll` and verifies the copy's hash before replacing anything;
- refuses to install if `OpenAL32_real.dll` already exists (indicating a prior or interrupted installation);
- records every file it wrote with a SHA-256 hash in `hyprlodr-install-manifest.json`;
- on uninstall, refuses to delete a proxy whose hash no longer matches the manifest and tells you to verify game files through Steam instead;
- removes only files it recorded; unknown files in `mods/` are left alone;
- prompts before making any change unless `--yes` is passed; `--dry-run` shows the plan.

The original library is never deleted by the installer. Uninstalling restores it by renaming `OpenAL32_real.dll` back to `OpenAL32.dll`.

Nothing in HYPRLODR modifies Steam's client, `steam_api64.dll`, DRM, or integrity checks, and nothing uploads game files anywhere. Diagnostics are read-only and produce hashes and PE metadata, not binaries.

## Competitive integrity

HYPER DEMON has online scoring. HYPRLODR's position:

- **Do not implement** score submission, leaderboard manipulation, or anything that affects other players.
- **Do not claim** modded play is leaderboard-safe. Selecting Steam Offline Mode does not establish that queued scores cannot be submitted later; no evidence for reliable isolation has been collected.
- **Do not patch Steam APIs or suppress integrity checks.**
- Real-game tests are currently limited to passive diagnostics — a mod that logs its own presence.

Before any gameplay-modifying feature ships, score submission behaviour must be observed and understood, and modded sessions must be isolated by a verified mechanism. Until then, documentation must describe the risk as unresolved.

Running mods on a scored account is a decision the user makes; the project does not present it as safe.

## Distribution

Mods are native binaries. A future catalogue must treat them as executables, not data: hash them, never auto-execute on metadata inspection, and surface the trust decision to the user. No remote catalogue exists yet.
