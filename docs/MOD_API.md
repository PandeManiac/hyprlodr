# Mod API (ABI 2)

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

State set during `initialize` is journaled: if initialization fails, every change is undone automatically. State written during `shutdown` is not journaled.

## Lifecycle guarantees

- `initialize` runs after every dependency's `initialize` returned success.
- `shutdown` runs in exact reverse initialization order, only for mods that initialized successfully.
- On failure, successfully initialized mods are shut down before the loader returns.
- The library is unloaded only after `shutdown` returns — never while your code may still be running.

## Packaging

A distributable mod is a directory (or ZIP, see `docs/TESTING.md`) containing:

```text
my-mod/
  manifest.json      id, name, version, author, api, platform, dependencies, entry
  native/<arch>/my-mod.dll
```

The manifest is metadata for installers and humans; the **descriptor in the DLL is authoritative** at runtime. Do not ship a manifest that disagrees with the descriptor — the installer validates that they match.

## Compatibility

ABI 2 is the current interface. There is no sandbox: a mod is trusted native code with the full privileges of the game process. See `docs/SECURITY.md` before distributing anything.
