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
- `DllMain(DLL_PROCESS_ATTACH)` records paths, calls `DisableThreadLibraryCalls`, installs lightweight main-image file/GL import shims, and creates a worker thread. The file-hook early phase performs bounded reads of the already mapped executable and import-slot protection changes only; it does not open the diagnostic file or enumerate modules under the loader lock. The worker opens diagnostics, completes the module walk, and handles mod loading after attach returns.
- Optional first-load diagnostics are enabled only by `HYPRLODR_FIRST_LOAD_DIAGNOSTICS=1`. Attach and loader callbacks reserve fixed-size records in a 1,024-entry static ring; they perform no file I/O, heap allocation, or formatting. The bootstrap worker opens per-run `hyprlodr-startup-<run-id>.jsonl` and `hyprlodr-files-<run-id>.jsonl` files, drains the early records, and starts a dedicated writer thread. Attach records use the same run ID and are also per-run, avoiding collisions with stale or overlapping process output. Startup and file-hook events share an atomic sequence and include process/thread IDs. A dropped-event record and attach counter make ring overflow explicit. Writer creation/open status is recorded in the attach report; if bootstrap itself cannot start, the offline coverage report marks the trace incomplete.
- The loader callback only reserves a ring slot and copies bounded fields. A diagnostic-only atomic guard serializes the single ring consumer while bootstrap and the periodic writer both request drains. The proxy writer is process-lifetime; it is not joined from `DllMain`, where waiting would be unsafe under the loader lock and the permanent callsite relay also prevents supported DLL unload. `hyprlodr_startup_diag_finish()` exists for safe non-loader-lock shutdown and is exercised by the fixture, not called from process detach.
- The worker reads `<game>/hyprlodr-config.json` for `{ "delay_ms": N }` (clamped to 0–60000; missing or malformed config falls back to 2500 ms) and an optional `"experimental_record_route": true` request. Routing defaults off. An opt-in request must also provide `experimental_record_route_manifest` and `experimental_record_route_archive` paths. Native preflight validates the supported executable and archive identities, resolver table/record identity, replacement extent and ciphertext hash, and expected mesh metadata. Immutable route state remains in proxy static storage. Early record interception is separately opt-in through `HYPRLODR_EARLY_RECORD_HOOK=1`; process-start attach verifies the mapped image anchors, static OpenAL32 import and empty executable TLS callback array, publishes the original-loader target, and installs the callsite hook disabled. Dynamic `LoadLibrary` attaches are rejected. The worker enables routing only after manifest validation and confirmation that the `asset/asset` redirect resolves to the manifest archive, then releases the archive-open gate. The main executable statically imports the decorated MSVC++ `_Fiopen` symbol, so its IAT slot is patched during attach; the compiled fixture verifies this import ownership with a stand-in `msvcp140.dll`. An intercepted `asset/asset` open waits up to 65 seconds for bootstrap publication. Timeout permanently pins archive opens to the original path and prevents later route activation; bootstrap failure also releases the gate to original behavior. This file-open gate does not itself prove that the first cold `hand2` lookup occurs after activation. `redirect_wait` and `file_open` records report wait and final open outcomes after the worker opens diagnostics; a successful open does not prove later read, decode or rendering.
- Evidence: `<game>/hyprlodr-attach.json` records PID, proxy directory, real executable path, ABI, and status; `<game>/hyprlodr-events.jsonl` records the mod lifecycle.
- `DLL_PROCESS_DETACH` with `reserved == NULL` (explicit unload) runs shutdown. Process termination shut-down runs no mod code.

## File interception (ABI 4)

A redirected file is only useful if the host controls file opening. The Windows proxy does, by patching **import slots** in the mapped main module: it parses the PE import directory, matches the DLL and function by name, saves the resolved address and points the slot at a shim. No game code is patched and no RVA from static analysis is used, so a game update cannot silently land a hook in the middle of an instruction.

Six targets are attempted independently: `CreateFileA` from `KERNEL32.dll`; `fopen`, `fopen_s`, `_wfopen`, and `_wfopen_s` from the UCRT API set; and MSVC++ `std::_Fiopen`. The pinned executable's import table directly imports decorated `_Fiopen` from `MSVCP140.dll`: the import slot belongs to `hyperdemon.exe`, and MSVCP140 supplies the target. The early main-image patch can therefore intercept this archive-open path before the bootstrap worker starts. A compiled Wine fixture imports that exact decorated symbol from a stand-in `msvcp140.dll` and exercises the gate; prior game captures independently observed the real archive open at this shim.

Patching the main module alone did not observe imports owned by other modules, so the worker also walks a bounded loaded-module list (`CreateToolhelp32Snapshot`) and patches imports there, skipping the proxy's own module. That walk is not needed for the executable's direct `_Fiopen` import. `_wfopen` converts its UTF-16 path to UTF-8 for registry matching and converts a replacement back to UTF-16 before calling the original; a path that does not convert is passed through unchanged.

Variable-size model routing remains experimental and is not installed by the
proxy. Ghidra and fresh disassembly of the pinned executable identify the
five-byte direct loader call at RVA `0x211f78` (`E8 A3 09 00 00`), and the call
instruction is included in the runtime build fingerprint. The reusable
`hyprlodr_record_route_invoke` helper changes only a request-local descriptor,
then restores its offset and size while preserving loader-owned fields. The managed interception lifecycle supports an explicitly opted-in dormant
install during verified process initialization, before the executable entry
point. Deferred routing is enabled only after manifest and archive-redirect
validation. The wrapper adds no loader locks, allocations, waits, or shared
mutable route data; unresolved cache and archive-stream serialization remain
properties of the existing game loader. A fingerprint match alone does not
make live patching safe. See
`docs/RE_MODEL.md` for the instruction evidence and remaining boundaries.

### Managed x64 callsite facility

`native/win32/callsite_hook.c` is a reusable, Windows-only installer for an
exact five-byte `CALL rel32`. Its relay search probes at most 64 nearby
allocation-granularity addresses, making failure bounded and fail-closed. It checks all five expected bytes, decodes and
optionally validates the original target, emits a direct relative call when the
replacement is reachable, and otherwise creates a nearby relay containing an
absolute `mov r10, imm64; jmp r10` (preserving the argument registers and RAX).
It changes page protection only for the
single page containing the call, flushes the instruction cache for relay and
site writes, restores protection, and rolls back the site bytes when a later
installation step fails. Cross-page call instructions are rejected.

The API requires `HYPRLODR_CALL_HOOK_INSTALL_QUIESCENT`. This is an explicit
caller assertion; it does not suspend or inspect process threads. HYPRLODR
uses it only when an explicit environment opt-in is present and Windows marks
the `DLL_PROCESS_ATTACH` as process initialization (`reserved != NULL`). The
proxy checks the supported executable anchors, static OpenAL32 import and
empty executable TLS callback array before accepting this point. A dynamic
attach (`reserved == NULL`) is rejected.
The fixture enforces the same distinction with a statically imported DLL and a
separate late-`LoadLibrary` case. This does not support arbitrary-time hot
installation.

After install, `hyprlodr_call_hook_set_enabled()` atomically changes logical
routing state. The replacement owns the disabled path and must call the saved
original target once. There is deliberately no physical unpatch or relay free:
callsite, relay, hook state, binding, route descriptor and callback code remain
valid until process exit, so an in-flight call cannot reach reclaimed memory.
Installation failures leave original call bytes in place; a relay allocated
before a later failure can remain reserved until exit. Repeating the same
installation is idempotent; a different installation against the same state is
rejected.

`native/win32/record_route_hook.c` is the game-specific wrapper. Its original
loader pointer is published before the patch; an absent binding or null route
therefore takes the allocation-free pass-through path instead of returning a
fabricated zero. The immutable route pointer is published atomically after
manifest validation. It reads the verified MSVC x64 `std::string` representation (inline/pointer at `+0`, size at
`+0x10`, capacity at `+0x18`) using bounded copies, accepts only the exact
five-byte name `hand2`, and passes the original name object and game context to
the loader. The binding and immutable route must be bound before installation
and outlive every call. The proxy binds a null route and original loader before
installing the dormant hook; the worker publishes the validated route later
and enables it only after confirming the archive redirect matches the manifest.
That gate holds the executable's known `_Fiopen` archive import before
bootstrap. The Wine fixture verifies this import relationship and gate ordering
with a stand-in runtime DLL. It does not prove that every alternate archive path
is intercepted or that the first cold `hand2` request cannot precede route
activation. A cold request completed while dormant could cache the original
model and later hits may bypass the callsite. The archive-open race has a
tested mechanism, while first-cold-model ordering remains a game-specific
runtime question.

The pre-entry fixture statically imports a proxy-like DLL whose `DllMain`
installs the real hook against an assembly-authored `CALL rel32` before the
executable entry function runs. It verifies dormant forwarding, later route
publication, route enable/disable without changing the instruction, no-opt-in,
wrong-byte rejection, failed bootstrap, and dynamic-load rejection. The
separate Wine executable fixture builds a real machine-code thunk containing a
`CALL rel32` and invokes it before and after interception. Its startup-order
case parks the only synthetic resource thread at an archive-open gate before
the callsite, installs and enables the hook, then releases the thread and
checks routing and metadata restoration. The pre-entry case proves the startup loader ordering for the fixture; the
archive gate alone is not the patch-safety mechanism. The fixture exercises direct
and relayed replacement, route-core integration, byte/target mismatch,
allocation/protection failure, reentrancy, concurrent calls and toggling, and
disablement during an in-flight call. It proves the facility's ABI and lifetime contract; the static-import fixture
also exercises process-start installation under Wine.

Safety properties:

- a target that is absent, malformed or unwritable is reported and skipped; the others still install and every mod still initializes;
- a failed install closes the log handle and reports why in `file_hook`/`file_hook_detail`, so a retry cannot append to a file nobody owns;
- the saved original function pointer is written, with a full fence, **before** the slot changes, so a game thread that reaches the shim immediately still has a valid target;
- the shim reads the loader and original pointers once into locals, so it never observes a partially updated pair;
- open records are appended with a fixed `FILE_APPEND_DATA` handle and an atomic sequence number, so concurrent game threads cannot corrupt the file or reuse a sequence;
- the redirect table is published with a release store and read with an acquire load, so a game thread that sees `redirects_published` sees a complete table, and unpublished registrations are invisible; the archive-open gate additionally waits for this worker lifecycle to finish before resolving the game's first archive open.

The import shims still do not reach file opens performed through dynamically resolved functions or direct lower-level system calls. For the archive path specifically, the controlled run observed `_Fiopen`; its `ok` field confirms a stream was returned, while later read/decode/render success requires separate evidence. See `docs/TESTING.md` and `docs/RE_MODEL.md`.

## Why OpenAL32 rather than a code hook

The executable statically imports `OpenAL32.dll`, so the Windows loader resolves it from the application directory before any game code runs — an address-free integration point that survives game updates, and the thing that gets the loader into the process at all. The file hook above is deliberately built on the same principle: symbol-resolved import slots. The new address-based callsite facility is fingerprinted and fixture-tested, but remains uninstalled in the game until process-wide quiescence is established; its wrapper adds no new loader synchronization or shared descriptor state. See `docs/COMPATIBILITY.md`.
