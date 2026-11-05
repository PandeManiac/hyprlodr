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

Before each `initialize()` the loader starts an undo journal. Every `state_set`, `state_remove`, `capability_register` and `file_redirect` records the previous value. If `initialize()` returns non-zero:

1. the journal is replayed backwards, removing or restoring exactly what that mod touched (a key the mod *removed* is re-inserted with its previous value and owner, because it is no longer in the table to be updated);
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
- `DllMain(DLL_PROCESS_ATTACH)` records paths, calls `DisableThreadLibraryCalls`, installs the lightweight main-image file/GL import shims, and creates a worker thread. It does not resolve or initialize mods, load mod DLLs, or invoke arbitrary mod code under the loader lock. The file shim opens its bounded diagnostic log while installed early; the complex module walk and mod loading remain on the worker.
- The worker reads `<game>/hyprlodr-config.json` for `{ "delay_ms": N }` (clamped to 0–60000; missing or malformed config falls back to 2500 ms). The configured delay is a startup pacing choice, not an ordering guarantee: at `delay_ms: 0`, the game can still reach its archive open before the worker has registered a redirect. To close that race, an intercepted `asset/asset` open waits until the worker publishes redirects or finishes initialization, then resolves the path. The wait is bounded at 65 seconds; on initialization failure or timeout the original path is opened. Other file opens never wait. `redirect_wait` records show the wait outcome when one occurs; `file_open` separately records whether the replacement path was selected and whether the underlying open succeeded. A successful open does not by itself prove that a later read, decode, or render succeeded.
- Evidence: `<game>/hyprlodr-attach.json` records PID, proxy directory, real executable path, ABI, and status; `<game>/hyprlodr-events.jsonl` records the mod lifecycle.
- `DLL_PROCESS_DETACH` with `reserved == NULL` (explicit unload) runs shutdown. Process termination shut-down runs no mod code.

## File interception (ABI 4)

A redirected file is only useful if the host controls file opening. The Windows proxy does, by patching **import slots** in the mapped main module: it parses the PE import directory, matches the DLL and function by name, saves the resolved address and points the slot at a shim. No game code is patched and no RVA from static analysis is used, so a game update cannot silently land a hook in the middle of an instruction.

Six targets are attempted independently: `CreateFileA` from `KERNEL32.dll`; `fopen`, `fopen_s`, `_wfopen`, and `_wfopen_s` from the UCRT API set; and MSVC++ `std::_Fiopen`. `_Fiopen` is the path used by the game's archive open and is patched by its decorated import name. An earlier capture before `_Fiopen` was added saw no asset opens; the controlled replacement run now confirms that the archive open reaches this shim.

Patching the main module alone proved insufficient: a run that patched the file targets there still logged no asset opens, because a module's import table belongs to that module. The installer therefore walks the loaded-module list (`CreateToolhelp32Snapshot`), parses each image's import directory and patches the same slots in every one, skipping the proxy's own module so its start-up I/O is never redirected or reported as game activity. The controlled real-game run reports `CreateFileA=patched(7) fopen=patched(1) fopen_s=patched(1) _wfopen=patched(1) _Fiopen=patched(1) modules=39`; `_wfopen_s` was absent. `_wfopen` converts its UTF-16 path to UTF-8 for registry matching and converts a replacement back to UTF-16 before calling the original; a path that does not convert is passed through unchanged.

Variable-size model routing remains experimental and is not installed by the
proxy. Ghidra and fresh disassembly of the pinned executable identify the
five-byte direct loader call at RVA `0x211f78` (`E8 A3 09 00 00`), and the call
instruction is included in the runtime build fingerprint. The reusable
`hyprlodr_record_route_invoke` helper changes only a request-local descriptor,
then restores its offset and size while preserving loader-owned fields. No
managed callsite interception lifecycle is implemented yet: neither concurrent
patch safety nor the game's cache/archive-stream synchronization has been
established. A fingerprint match alone does not make live patching safe. See
`docs/RE_MODEL.md` for the instruction evidence and remaining boundaries.

Safety properties:

- a target that is absent, malformed or unwritable is reported and skipped; the others still install and every mod still initializes;
- a failed install closes the log handle and reports why in `file_hook`/`file_hook_detail`, so a retry cannot append to a file nobody owns;
- the saved original function pointer is written, with a full fence, **before** the slot changes, so a game thread that reaches the shim immediately still has a valid target;
- the shim reads the loader and original pointers once into locals, so it never observes a partially updated pair;
- open records are appended with a fixed `FILE_APPEND_DATA` handle and an atomic sequence number, so concurrent game threads cannot corrupt the file or reuse a sequence;
- the redirect table is published with a release store and read with an acquire load, so a game thread that sees `redirects_published` sees a complete table, and unpublished registrations are invisible; the archive-open gate additionally waits for this worker lifecycle to finish before resolving the game's first archive open.

The import shims still do not reach file opens performed through dynamically resolved functions or direct lower-level system calls. For the archive path specifically, the controlled run observed `_Fiopen`; its `ok` field confirms a stream was returned, while later read/decode/render success requires separate evidence. See `docs/TESTING.md` and `docs/RE_MODEL.md`.

## Why OpenAL32 rather than a code hook

The executable statically imports `OpenAL32.dll`, so the Windows loader resolves it from the application directory before any game code runs — an address-free integration point that survives game updates, and the thing that gets the loader into the process at all. The file hook above is deliberately built on the same principle: symbol-resolved import slots, never code addresses. Address-based hooks would need fingerprints and compatibility gates; none are used yet. See `docs/COMPATIBILITY.md`.
