# Architecture

## Components

```text
                      +---------------------------+
  game process  --->  | OpenAL32.dll (HYPRLODR)   |  proxy DLL, 175 PE forwarders
                      |   DllMain -> worker thread|
                      |   + hyprlodr_loader       |
                      +---------------------------+
                                 |
                                 v
                        mods/*.dll (ABI 3)
```

1. **Public ABI** (`native/include/hyprlodr/plugin.h`) — the only contract between loader and mods. A mod exports one symbol, `hyprlodr_plugin_query`, returning a static descriptor.
2. **Loader** (`native/src/loader.c`) — discovery, validation, ordering, lifecycle, event log. No knowledge of any specific game or mod.
3. **Hosts** — `native/src/host.c` (synthetic test host, JSON summary) and `native/win32/proxy.c` (in-game bootstrap inside the OpenAL32 proxy).
4. **Tooling** (`tools/`) — diagnostics, installation with rollback, proxy `.def` generation, Windows build/test drivers.

The loader does not know which host runs it. The synthetic host and the real game use the same code path, which is why the Wine host test is meaningful evidence about the game path (minus the game itself).

## Loader phases

```c
hyprlodr_loader_create();
hyprlodr_loader_add_dir();        /* repeatable */
hyprlodr_loader_disable();        /* installed but inactive */
hyprlodr_loader_set_log();
hyprlodr_loader_resolve();        /* load libraries, validate, order — no mod code runs */
hyprlodr_loader_init();           /* initialize in order, roll back on failure */
hyprlodr_loader_shutdown();       /* reverse order, idempotent */
hyprlodr_loader_destroy();        /* unload libraries, free memory */
```

Splitting resolve from init matters: metadata errors (bad ABI, duplicate id, cycle, missing dependency) are reported before any third-party code executes.

### Discovery

Paths from every added directory are collected, **sorted lexicographically by full path**, then loaded. Sorting makes duplicate-id reporting and error ordering independent of filesystem enumeration order. `hyprlodr_loader_set_seed()` additionally shuffles the in-memory plugin array with a seeded Fisher–Yates shuffle; the test suite uses that to prove the resolved order never depends on discovery order.

### Validation before execution

For each library: the entry symbol must exist; the descriptor pointer must be valid; `struct_size` must cover the ABI-1 fields; `abi_version` must equal the host ABI; `id` must be a lowercase `[a-z0-9._-]` string under 128 bytes; version, `initialize` and `shutdown` must be non-NULL; counts must be consistent with their arrays; ids must be unique.

Descriptor extension is append-only and guarded by `HYPRLODR_DESCRIPTOR_HAS`: a future ABI can add fields without the loader reading past an older descriptor.

### Ordering

Kahn-style selection over enabled mods. Each round scans plugins in **mod-id order** and picks the first whose dependencies are all satisfied and emitted. Consequences:

- dependencies always initialize before dependents;
- independent mods are ordered lexicographically by id (`00.final` sorts before `a.palette`, which is exactly the trap the sample suite encodes);
- a round that emits nothing means a cycle — reported once with the remaining mod ids.

### Failure handling

Before each `initialize()` the loader starts an undo journal. Every `state_set`, `state_remove` and `capability_register` records the previous value. If `initialize()` returns non-zero:

1. the journal is replayed backwards, removing or restoring exactly what that mod touched;
2. the resolved order is truncated to the successfully initialized prefix;
3. those mods are shut down in reverse order with normal `shutdown` events;
4. an `error` event reports the failure code.

Dependents never start, because initialization stops at the failure. The loader never leaves half-initialized state visible to the next process.

### Unloading

Libraries are unloaded only in `destroy()`, after `shutdown()` has returned for every mod. Mods may own threads or heap; the loader does not attempt live unload while references may exist. In-game, mods stay loaded until process exit.

## Windows target layer

The shipped Windows artifacts depend on `kernel32.dll` only:

- `native/win32/crt/*.h` — declarations for the libc subset used (`malloc`, `snprintf`, `fopen`, `qsort`, ...).
- `native/win32/crt_impl.c` — that subset implemented on HeapAlloc/CreateFileA/WriteFile, compiled with `-fno-builtin`.
- `native/win32/kernel32.def` — converted to an import library by `llvm-dlltool`.
- `native/win32/entry.c` — rebuilds `argv` from `GetCommandLineA` (the Windows loader passes `(HINSTANCE, HINSTANCE, LPSTR)` to the entry point, not `argc/argv`).

No CRT dependency means no CRT start-up requirements, no msvcrt version skew, and identical behaviour under Wine, Proton and Windows.

## In-game bootstrap

`native/win32/proxy.c` is compiled into the replacement `OpenAL32.dll`.

- All 175 named OpenAL exports are PE **forwarders** to `OpenAL32_real.<name>`, generated by `tools/gen_proxy_def.py` from the shipped DLL's export table. Nothing about audio is reimplemented.
- `DllMain(DLL_PROCESS_ATTACH)` records paths, calls `DisableThreadLibraryCalls`, and creates a worker thread. Creating a thread is the only work done under the loader lock; no mod code, no `LoadLibrary` of mods, no file I/O for mods.
- The worker sleeps before resolving mods so the game can finish its own start-up. The default is 2500 ms; `<game>/hyprlodr-config.json` overrides it with `{ "delay_ms": N }` (clamped to 0–60000, and a missing or malformed file falls back to the default rather than failing to attach). The effective value is recorded as `delay_ms` in the attach record. A machine that reaches its first frame quickly, or a process that exits before the delay elapses, needs `delay_ms: 0` set explicitly — otherwise no mod-init events are ever written.
- Evidence: `<game>/hyprlodr-attach.json` records PID, proxy directory, real executable path, ABI, and status; `<game>/hyprlodr-events.jsonl` records the mod lifecycle.
- `DLL_PROCESS_DETACH` with `reserved == NULL` (explicit unload) runs shutdown. Process termination shut-down runs no mod code.

## Why OpenAL32 rather than a hook

The executable statically imports `OpenAL32.dll`, so the Windows loader resolves it from the application directory before any game code runs — a hook-free, address-free integration point that survives game updates. Address-based hooks would need fingerprints and compatibility gates; none are used yet. See `docs/COMPATIBILITY.md`.
