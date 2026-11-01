# Mod API (ABI 5)

A HYPRLODR mod is a native shared library (`mods/<name>.dll` on Windows, `.so` on Linux) exporting exactly one symbol.

```c
#include "hyprlodr/plugin.h"

static int mod_initialize(const hyprlodr_host_api *host) {
    if (host->capability_register(host->context, "example.overlay", "example.mod") != 0) return -1;
    host->log(host->context, "info", "example.mod", "hello from inside the game");
    return 0;
}

static void mod_shutdown(const hyprlodr_host_api *host) {
    host->log(host->context, "info", "example.mod", "bye");
}

static const hyprlodr_plugin_descriptor descriptor = {
    (uint32_t)sizeof descriptor,
    HYPRLODR_ABI_VERSION,
    "example.mod", "1.0.0",
    NULL, 0,                       /* dependencies */
    mod_initialize, mod_shutdown,
    NULL, 0,                       /* conflicts */
    "Example mod", "Does an example thing.", "you",
    HYPRLODR_PLUGIN_FLAG_DIAGNOSTIC,
};

HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version) {
    return host_abi_version == HYPRLODR_ABI_VERSION ? &descriptor : NULL;
}
```

Build with the header on the include path and no extra libraries; the samples in `samples/` are complete working examples.

## Persistent state (ABI 3)

`state_dir()` returns an absolute directory reserved for the calling mod. The host creates it **before** `initialize()` runs, so a mod can read its settings on first startup without any setup step:

```c
if (HYPRLODR_HOST_API_HAS(host, state_dir) && host->state_dir) {
    const char *dir = host->state_dir(host->context);
    if (dir && *dir) {
        /* read/write <dir>/settings.txt */
    }
}
```

- The path is `<state root>/<mod-id>`. In the game the proxy uses `<game>/hyprlodr-state`, so a mod sees e.g. `S:\steamapps\common\hyperdemon\hyprlodr-state\my.mod`.
- It is `NULL` when the host was started without a state root, and for ABI 2 hosts. Always check.
- The directory is user data: it survives uninstall, upgrades and process restarts. Files written in one session are readable in the next (covered by the `state_dir_persists_across_runs` scenario). Never delete files you did not create.
- `state_set`/`state_get` remain the in-memory channel for cross-mod coordination during one session; they are not persisted. The loader emits a `state_dir` event when it reserves a directory, and `state_dir_failed` if it cannot.

## File redirection (ABI 4)

`file_redirect()` asks the host to open a different file whenever the game opens a path ending in a suffix you own:

```c
if (HYPRLODR_HOST_API_HAS(host, file_redirect) && host->file_redirect) {
    /* the game's own asset path -> a file this mod ships */
    if (host->file_redirect(host->context, "data/level1.asset", "C:/mods/mine/level1.asset") != 0) return -1;
}
```

- The suffix must begin at a path separator (`level1.asset` matches `...\\data\\level1.asset`, not `...\\mylevel1.asset`). Matching folds ASCII case and treats `\` and `/` alike, because the game mixes both.
- A duplicate suffix is refused: the first registration wins and stays, so two mods cannot silently fight over one path.
- Redirects are recorded as `file_redirect` events and become active only after **every** mod has initialized, so a half-initialized set never changes what the game reads. On initialization failure the journal removes them again; on shutdown they stop applying immediately.
- Which opens the host can redirect depends on that host. The Linux test host exposes `--redirect-probe PATH`; the Windows proxy patches the game's own import slots. Measure, do not assume: the shipped `hyperdemon.exe` performed no narrow-path opens at all in a real run (see `docs/TESTING.md`), so redirection is currently proven against the stand-in host and not yet against the game's asset loader.

## Game image access (ABI 5)

A mod may read the running game image, but only when the host has proved the executable is the build the verifier describes:

```c
if (HYPRLODR_HOST_API_HAS(host, image_read) && host->image_read && host->build_matches(host->context)) {
    unsigned char header[8];
    if (host->image_read(host->context, 0, header, sizeof header) == 0) {
        /* header[0..1] == "MZ" */
    }
    size_t size = host->image_size(host->context);  /* 0 when no image is offered */
}
```

- `build_matches()` is 0 unless every fingerprint anchor matched, in which case **every read is refused**. This is the gate that stops a mod acting on offsets from a different executable.
- A read is refused when the range is not entirely inside the image (including a length that would wrap), or the length is zero. Refusal is a normal return value, not a crash: treat it as "not available".
- The API is read-only. Nothing here lets a mod patch the game, and nothing is exposed unless the host supplies an image at all — the synthetic test host returns 0 from `image_size()`.

## Rules

- **One export.** `hyprlodr_plugin_query` must return a pointer to storage that lives for the whole process (usually `static`). All descriptor strings are owned by the mod and must also remain valid.
- **ABI negotiation.** Return `NULL` if the host's ABI version is unsupported. Returning a descriptor whose `abi_version` differs from the host's is an error and the mod is rejected.
- **Descriptor size.** Set `struct_size` to `sizeof descriptor`. The loader reads only fields the size covers, so newer fields are optional for older hosts and vice versa.
- **Mod ids** are lowercase `[a-z0-9._-]`, at most 127 characters, unique across the installation.
- **Versions** are numeric dot versions (`1.2.3`, `1.2`, `1`). Pre-release/build suffixes after `-`/`+` are ignored for ordering.
- **Return value.** `initialize` returns `0` on success. Any other value fails the mod, rolls back everything it changed, and stops all dependent mods from starting.
- **Never assume load order** beyond your declared dependencies.

## Dependencies and constraints

Dependency entries accept an optional constraint after `@`:

```c
static const char *const dependencies[] = {
    "z.foundation",            /* any version */
    "a.palette@>=1.2.0",       /* comparison: >= <= > < = */
    "m.overlay@^1.0.0",        /* same major version */
    "b.audio@~1.2.0",          /* same major and minor */
};
```

Missing, disabled, or version-incompatible dependencies are reported before any mod initializes.

## Conflicts

`conflicts` entries use the same syntax as dependencies, without the dependency relationship: if a matching mod is present and enabled, the loader reports a conflict and refuses to initialize anything.

## Host services

All calls take `host->context`. Every function may return failure; treat failure as fatal for your initialization unless you handle it explicitly.

| Function | Behaviour |
|---|---|
| `log(context, level, mod_id, message)` | One JSON event; `level` is free-form (`info`, `warn`, `error`). |
| `state_set(context, key, value)` | Create or replace a keyed string. Keys are global to the process; last writer wins. |
| `state_get(context, key)` | Returns the stored string or `NULL`. Valid until that key is next modified. |
| `state_remove(context, key)` | Remove a key; returns non-zero if absent. |
| `capability_register(context, name, provider_id)` | Publish a capability; duplicate names fail. |
| `capability_has(context, name)` | Non-zero if the capability exists. |
| `is_loaded(context, mod_id)` | Non-zero if that mod is currently initialized. |
| `file_redirect(context, suffix, replacement_path)` | Open `replacement_path` instead whenever the game opens a path ending in `suffix` (ABI 4). |
| `image_size(context)` | Bytes of the running image a mod may read; 0 when the host offers none (ABI 5). |
| `build_matches(context)` | Non-zero only when the image is the exact verified build (ABI 5). |
| `image_read(context, offset, dst, length)` | Copy bytes from the image; refused when the build is unverified or the range is outside it (ABI 5). |

State set during `initialize` is journaled: if initialization fails, every change is undone automatically, including state the failed mod *deleted* — a removed entry is re-inserted with its previous value and owner (scenario `state_remove_rolled_back_on_failure`). State written during `shutdown` is not journaled.

## Lifecycle guarantees

- `initialize` runs after every dependency's `initialize` returned success.
- `shutdown` runs in exact reverse initialization order, only for mods that initialized successfully.
- On failure, successfully initialized mods are shut down before the loader returns.
- The library is unloaded only after `shutdown` returns — never while your code may still be running.

## Packaging

A distributable mod is a ZIP with `manifest.json` at the root (schema 1):

```text
manifest.json
native/<platform>/<id>.dll      or .so on linux-x86_64
assets/...                      optional payload (opaque to the loader today)
config/...                      optional defaults
README.md, LICENSE              optional
```

```json
{
  "schema": 1,
  "id": "example.mod",
  "name": "Example mod",
  "version": "1.0.0",
  "author": "you",
  "description": "What it does",
  "api": 2,
  "platform": "windows-x86_64",
  "entry": "native/windows-x86_64/example.mod.dll",
  "dependencies": ["z.foundation@^1.0.0"],
  "optional_dependencies": [],
  "conflicts": [],
  "capabilities": ["example.overlay"]
}
```

Build and install:

```bash
python3 tools/hyprl_package.py pack my-mod/ out/example.mod.zip
python3 tools/hyprl_package.py validate out/example.mod.zip
python3 tools/hyprl_package.py install out/example.mod.zip \
    --mods-dir "<game>/mods" --host build-win/hyprlodr-host.exe
```

The manifest is metadata for installers and humans; the **descriptor in the binary is authoritative** at runtime. Pass `--host` and the installer loads the library through `hyprlodr-host --describe` (descriptor query only, no initialization) and refuses the package if id, version or dependency list disagree.

Validation is defensive: absolute paths, `..` segments, backslashes, symlinks, duplicate or case-colliding names, files outside `native/`, `assets/` and `config/`, oversized entries and suspicious compression ratios are all rejected before anything is written.

## Worked example: `example.game-info`

`examples/game_info.c` is the first mod that reads the game rather than only running beside it. It is the template for any data-driven mod:

```c
if (!host->build_matches(host->context)) return 0;          /* never read an unknown build */
unsigned char bytes[12];
if (host->image_read(host->context, RVA, bytes, sizeof bytes) != 0) return 0;
if (fnv1a64(bytes, sizeof bytes) != EXPECTED_FNV) { /* log and stop */ }
```

It reads the loader's own `resource_path` anchor (RVA `0x36f640`, 12 bytes), recomputes its FNV-1a 64 hash and compares it with the constant recorded during reverse engineering. On the analysed build it logs `resource path anchor verified at rva 36f640` and publishes the `example.game-info` capability; on anything else it logs a mismatch or a refusal and reads nothing further. It logs a **hash, never the bytes**, so evidence stays free of game content.

Verified in the real game on 2026-11-01: `image 4214784 bytes verified` followed by `resource path anchor verified at rva 36f640`, 7/7 mods initialized, 8/8 anchors. Evidence: `docs/evidence/2026-11-01-example-game-info.json`.

Two loader scenarios run the same Linux build against synthetic images: a 64-byte image (read refused, run still succeeds) and a 3.4 MB image of `A` bytes (read succeeds, hash comparison rejects it).

## Compatibility

ABI 5 is the current interface. There is no sandbox: a mod is trusted native code with the full privileges of the game process. See `docs/SECURITY.md` before distributing anything.

## Worked example: `example.uptime`

`examples/uptime.c` is a complete mod rather than a test fixture. Copy it as a starting point. It shows the four things most mods need first:

- a **versioned dependency** (`z.foundation@^1.0.0`) so the loader orders it correctly;
- **persistent storage** through `state_dir()`, incrementing a launch counter and writing a last-run marker;
- **a capability** (`example.uptime`) other mods can check with `capability_has`;
- **logging**, which appears in `<game>/hyprlodr-events.jsonl`.

It also carries two lessons that cost real time: the Windows target's minimal CRT has no `fscanf` (parse with `fgets`), and GCC's `-Wformat-truncation` rejects `%s` of an unbounded path into a small buffer, so log the number, not the path.

```bash
# Linux .so (compiled with the project's strict warnings)
cmake -S . -B build -G Ninja && cmake --build build      # -> build/examples/example.uptime.so

# Windows .dll plus a real package, built by the normal Windows build
tools/build_windows.sh build-win "/path/to/game/OpenAL32.dll"
#   -> build-win/examples/example.uptime.dll
#   -> build-win/examples/example.uptime.zip   (manifest from examples/package/manifest.json)

python3 tools/hyprl_package.py validate build-win/examples/example.uptime.zip
python3 tools/hyprl_package.py install   build-win/examples/example.uptime.zip --mods-dir "/path/to/game/mods"
```

Verified in the real game on 2026-10-31 with `delay_ms: 0`: six mods initialized in order `z.foundation, a.palette, b.audio, example.uptime, m.overlay, 00.final`, the example logged `launch 1 of 1.0.0 recorded` and `dependency z.foundation is present`, and `<game>/hyprlodr-state/example.uptime/` contained `launches.txt` and `last-run.txt`. Evidence: `docs/evidence/2026-10-31-example-mod.json`.

Note it is built into `build/examples/`, not `build/mods/`: the loader suite asserts exactly the five sample mods, so an extra library there would change those expectations.
