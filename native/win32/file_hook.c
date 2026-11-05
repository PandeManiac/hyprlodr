/*
 * In-game file interception (ABI 4).
 *
 * Symbol-resolved import patching: for each target the loader reads the mapped
 * main module's import directory, finds the slot by DLL + function name, saves
 * the resolved address and points the slot at a shim. No game code is patched
 * and no address from static analysis is used.
 *
 * Targets, chosen from the executable's real import table:
 *   KERNEL32.dll                     CreateFileA  (Win32 call sites)
 *   api-ms-win-crt-stdio-l1-1-0.dll  fopen        (narrow CRT opens)
 *   api-ms-win-crt-stdio-l1-1-0.dll  fopen_s
 *   api-ms-win-crt-stdio-l1-1-0.dll  _wfopen      (wide CRT opens)
 *   api-ms-win-crt-stdio-l1-1-0.dll  _wfopen_s
 *
 * CreateFileW was attempted and removed: patching a second module's
 * KERNEL32!CreateFileW slot (wprobe.dll) terminated the stand-in process with
 * no crash record and no file-log entry, so the wide Win32 shim is unproven and
 * not shipped. The same wide conversion is proven for _wfopen over the CRT
 * entry points, which is the form the game actually imports.
 *
 * Measured gap that motivated the CRT targets: a real run patched CreateFileA
 * and observed zero opens while five mods initialized - the game's asset I/O
 * never passes through that import. hyperdemon.exe imports fopen/fopen_s and
 * _wfopen from the UCRT API set, and only CreateFileA from KERNEL32. A later
 * run patched the two narrow CRT slots and still logged zero opens, which points
 * the asset path at the wide entry point; _wfopen converts its UTF-16 path to
 * UTF-8 for registry matching and converts a replacement back to UTF-16 before
 * calling the original.
 *
 * Failure policy: a target that is absent, malformed or unwritable is reported
 * and skipped; the remaining targets still install and every mod still starts.
 * Nothing is patched unless the PE structures actually describe the slot.
 */
#define HYPRLODR_MINIMAL_WIN32 1

#include "hypr_win32.h"
#include "file_hook.h"
#include "startup_diag.h"

#include <stdio.h>
#include <string.h>

#define HYPRLODR_IMPORT_DESCRIPTOR_BYTES 20u
#define HYPRLODR_SECTION_HEADER_BYTES 40u
#define HYPRLODR_MAX_IMPORT_DESCRIPTORS 256u
#define HYPRLODR_MAX_IMPORT_SLOTS 4096u
/* Bound on the module walk, so a pathological process cannot spin here. */
#define HYPRLODR_MAX_HOOK_MODULES 64u
/* An unbounded log would let a streaming game fill the disk. */
#define HYPRLODR_FILE_LOG_LIMIT 4096
/* A configured bootstrap delay can be as long as 60 seconds. Leave extra time
 * for mod discovery and initialization, but never let one game file open wait
 * forever if the worker failed to start or hung. */
#ifndef HYPRLODR_REDIRECT_WAIT_LIMIT_MS
#define HYPRLODR_REDIRECT_WAIT_LIMIT_MS 65000u
#endif

typedef HANDLE(WINAPI *create_file_fn)(LPCSTR name, DWORD access, DWORD share, void *security,
                                       DWORD disposition, DWORD flags, HANDLE template_file);
/* MSVC x64 CRT entry points use the platform calling convention; these are
 * passed through untouched, so the exact typedef only has to match the ABI. */
typedef FILE *(*fopen_fn)(const char *name, const char *mode);
typedef int (*fopen_s_fn)(FILE **out, const char *name, const char *mode);
typedef FILE *(*wfopen_fn)(const wchar_t *name, const wchar_t *mode);
/* MSVC++ stream open: exported by MSVCP140.dll and imported by the game. It is
 * the narrow (char) entry point, so no wide conversion is needed. */
typedef FILE *(*fiopen_fn)(const char *name, int mode, int prot);
typedef int (*wfopen_s_fn)(FILE **out, const wchar_t *name, const wchar_t *mode);

typedef struct {
    const uint8_t *base;
    const uint8_t *sections;
    unsigned section_count;
    uint32_t import_rva;
} pe_layout;

static create_file_fn g_original;
static fopen_fn g_original_fopen;
static fopen_s_fn g_original_fopen_s;
static wfopen_fn g_original_wfopen;
static fiopen_fn g_original_fiopen;
static wfopen_s_fn g_original_wfopen_s;
static hyprlodr_loader *g_loader;
static volatile int g_redirects_ready;
static volatile unsigned g_asset_waiters;
static volatile LONG g_archive_gate_state = HYPRLODR_ARCHIVE_GATE_PENDING;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static unsigned g_logged;
static unsigned g_event_sequence;
/* Tick when the hook was installed, so every record carries a relative time:
 * a long session then says *when* the game touches a file, not just that it did. */
static DWORD g_start_tick;
static int g_installed;
/* Set once the main module has been patched from DllMain, and once the module
 * walk has completed, so each phase runs at most once. */
static int g_started;
static int g_complete;
static const char *g_status = "not_attempted";
/* Which targets were patched, so a host reports exactly what is intercepted. */
static char g_detail[320] = "";

/* A failed install must not leave a log handle open, or a later retry would
 * append to a file nobody owns. */
static int fail_install(const char *status) {
    g_status = status;
    /* Keep the diagnostic handle alive for any shim already in flight. */
    return -1;
}

static uint16_t read_u16(const uint8_t *bytes) {
    uint16_t value;
    memcpy(&value, bytes, sizeof value);
    return value;
}

static uint32_t read_u32(const uint8_t *bytes) {
    uint32_t value;
    memcpy(&value, bytes, sizeof value);
    return value;
}

static uint64_t read_u64(const uint8_t *bytes) {
    uint64_t value;
    memcpy(&value, bytes, sizeof value);
    return value;
}

static int equals_ignore_case(const char *a, const char *b) {
    while (*a && *b) {
        char left = *a, right = *b;
        if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z') right = (char)(right - 'A' + 'a');
        if (left != right) return 0;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/* Case-insensitive substring test; import DLL names vary by contract version. */
static int contains_ignore_case(const char *haystack, const char *needle) {
    size_t length = strlen(needle);
    if (length == 0) return 1;
    for (const char *cursor = haystack; *cursor; ++cursor) {
        size_t index = 0;
        while (index < length && cursor[index]) {
            char left = cursor[index], right = needle[index];
            if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
            if (right >= 'A' && right <= 'Z') right = (char)(right - 'A' + 'a');
            if (left != right) break;
            ++index;
        }
        if (index == length) return 1;
    }
    return 0;
}

/* In a loaded image an RVA is already an offset from the base. Returns NULL when
 * the RVA is outside the image sections. */
static const uint8_t *rva_to_ptr(const pe_layout *pe, uint32_t rva) {
    for (unsigned index = 0; index < pe->section_count; ++index) {
        const uint8_t *header = pe->sections + (size_t)index * HYPRLODR_SECTION_HEADER_BYTES;
        uint32_t virtual_size = read_u32(header + 8);
        uint32_t virtual_address = read_u32(header + 12);
        uint32_t raw_size = read_u32(header + 16);
        uint32_t span = virtual_size > raw_size ? virtual_size : raw_size;
        if (rva >= virtual_address && rva < virtual_address + span) return pe->base + rva;
    }
    return NULL;
}

static void escape_into(char *destination, size_t capacity, const char *source) {
    size_t used = 0;
    for (const char *cursor = source ? source : ""; *cursor && used + 2u < capacity; ++cursor) {
        if (*cursor == '"' || *cursor == '\\') destination[used++] = '\\';
        destination[used++] = *cursor;
    }
    destination[used] = '\0';
}

static int is_asset_archive_path(const char *name);

static void record_open(const char *requested, const char *opened, const char *via, int redirected,
                        int ok) {
    if (is_asset_archive_path(requested))
        hyprlodr_startup_diag_emit_once(HYPRLODR_DIAG_ARCHIVE_OPEN, 0u, 0u, 0u,
            (uint32_t)hyprlodr_file_hook_archive_gate_state(),
            (redirected ? 1u : 0u) | (ok ? 2u : 0u), 0u, 0u, opened);
    HANDLE log = __atomic_load_n(&g_log, __ATOMIC_ACQUIRE);
    if (log == INVALID_HANDLE_VALUE) return;
    char requested_json[768];
    char opened_json[768];
    char line[2048];
    DWORD written = 0;
    /* Game threads open files concurrently. Reserve the sequence number
     * atomically so two records can never claim the same one, and stop writing
     * once the cap is reached. */
    unsigned open_index = __atomic_add_fetch(&g_logged, 1u, __ATOMIC_RELAXED);
    if (open_index > (unsigned)HYPRLODR_FILE_LOG_LIMIT) return;
    unsigned sequence = __atomic_add_fetch(&g_event_sequence, 1u, __ATOMIC_RELAXED);
    escape_into(requested_json, sizeof requested_json, requested);
    escape_into(opened_json, sizeof opened_json, opened);
    DWORD start_tick = __atomic_load_n(&g_start_tick, __ATOMIC_ACQUIRE);
    unsigned long elapsed_ms = start_tick ? (unsigned long)(GetTickCount() - start_tick) : 0ul;
    snprintf(line, sizeof line,
             "{\"seq\":%u,\"t_ms\":%lu,\"thread\":%lu,\"event\":\"file_open\","
             "\"via\":\"%s\",\"redirected\":%s,\"ok\":%s,"
             "\"requested\":\"%s\",\"opened\":\"%s\"}\n",
             sequence, elapsed_ms, (unsigned long)GetCurrentThreadId(), via,
             redirected ? "true" : "false", ok ? "true" : "false", requested_json, opened_json);
    /* The handle is opened for append, so each WriteFile lands at end-of-file
     * without a lock: game threads may interleave records but never corrupt the
     * file pointer, and no kernel32 interlocked import is needed. */
    WriteFile(log, line, (DWORD)strlen(line), &written, NULL);
}

static int is_asset_archive_path(const char *name) {
    static const char suffix[] = "asset/asset";
    size_t length = name ? strlen(name) : 0u;
    const size_t suffix_length = sizeof suffix - 1u;
    if (length < suffix_length) return 0;
    const char *tail = name + length - suffix_length;
    for (size_t index = 0; index < suffix_length; ++index) {
        unsigned char actual = (unsigned char)tail[index];
        unsigned char expected = (unsigned char)suffix[index];
        if (actual >= 'A' && actual <= 'Z') actual = (unsigned char)(actual + ('a' - 'A'));
        if (actual == '\\') actual = '/';
        if (actual != expected) return 0;
    }
    return length == suffix_length || tail[-1] == '/' || tail[-1] == '\\';
}

static void record_redirect_wait(const char *requested, const char *stage, DWORD waited_ms) {
    HANDLE log = __atomic_load_n(&g_log, __ATOMIC_ACQUIRE);
    if (log == INVALID_HANDLE_VALUE) return;
    char requested_json[768];
    char line[1200];
    DWORD written = 0;
    escape_into(requested_json, sizeof requested_json, requested);
    unsigned sequence = __atomic_add_fetch(&g_event_sequence, 1u, __ATOMIC_RELAXED);
    DWORD start_tick = __atomic_load_n(&g_start_tick, __ATOMIC_ACQUIRE);
    unsigned long elapsed_ms = start_tick ? (unsigned long)(GetTickCount() - start_tick) : 0ul;
    snprintf(line, sizeof line,
             "{\"seq\":%u,\"event\":\"redirect_wait\",\"stage\":\"%s\",\"t_ms\":%lu,"
             "\"thread\":%lu,\"waited_ms\":%lu,\"requested\":\"%s\"}\n",
             sequence, stage, elapsed_ms, (unsigned long)GetCurrentThreadId(), (unsigned long)waited_ms,
             requested_json);
    WriteFile(log, line, (DWORD)strlen(line), &written, NULL);
}

static void record_hook_install(const char *phase, unsigned patched) {
    HANDLE log = __atomic_load_n(&g_log, __ATOMIC_ACQUIRE);
    if (log == INVALID_HANDLE_VALUE) return;
    char line[512];
    DWORD written = 0;
    unsigned sequence = __atomic_add_fetch(&g_event_sequence, 1u, __ATOMIC_RELAXED);
    DWORD start_tick = __atomic_load_n(&g_start_tick, __ATOMIC_ACQUIRE);
    unsigned long elapsed_ms = start_tick ? (unsigned long)(GetTickCount() - start_tick) : 0ul;
    snprintf(line, sizeof line,
             "{\"seq\":%u,\"event\":\"file_hook_install\",\"phase\":\"%s\",\"t_ms\":%lu,"
             "\"thread\":%lu,\"patched_targets\":%u}\n",
             sequence, phase, elapsed_ms, (unsigned long)GetCurrentThreadId(), patched);
    WriteFile(log, line, (DWORD)strlen(line), &written, NULL);
}

static void wait_for_asset_redirects(const char *name) {
    if (!is_asset_archive_path(name) ||
        __atomic_load_n(&g_redirects_ready, __ATOMIC_ACQUIRE))
        return;
    DWORD started = GetTickCount();
    __atomic_add_fetch(&g_asset_waiters, 1u, __ATOMIC_RELAXED);
    hyprlodr_startup_diag_emit_once(HYPRLODR_DIAG_ARCHIVE_WAIT, 0u, 0u, 0u,
        (uint32_t)hyprlodr_file_hook_archive_gate_state(), 0u, 0u, 0u, name);
    record_redirect_wait(name, "begin", 0u);
    for (;;) {
        LONG gate_state = __atomic_load_n(&g_archive_gate_state, __ATOMIC_ACQUIRE);
        if (gate_state == HYPRLODR_ARCHIVE_GATE_READY ||
            gate_state == HYPRLODR_ARCHIVE_GATE_FALLBACK) {
            record_redirect_wait(name,
                                 gate_state == HYPRLODR_ARCHIVE_GATE_READY ? "ready" : "fallback",
                                 GetTickCount() - started);
            __atomic_sub_fetch(&g_asset_waiters, 1u, __ATOMIC_RELAXED);
            return;
        }
        DWORD waited = GetTickCount() - started;
        if (waited >= HYPRLODR_REDIRECT_WAIT_LIMIT_MS) {
            LONG expected = HYPRLODR_ARCHIVE_GATE_PENDING;
            if (__atomic_compare_exchange_n(&g_archive_gate_state, &expected,
                                             HYPRLODR_ARCHIVE_GATE_FALLBACK, 0,
                                             __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                record_redirect_wait(name, "timeout", waited);
                __atomic_sub_fetch(&g_asset_waiters, 1u, __ATOMIC_RELAXED);
                return;
            }
            if (expected == HYPRLODR_ARCHIVE_GATE_ACTIVATING) {
                expected = HYPRLODR_ARCHIVE_GATE_ACTIVATING;
                if (__atomic_compare_exchange_n(&g_archive_gate_state, &expected,
                                                 HYPRLODR_ARCHIVE_GATE_FALLBACK, 0,
                                                 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                    record_redirect_wait(name, "timeout_activation_aborted", waited);
                    __atomic_sub_fetch(&g_asset_waiters, 1u, __ATOMIC_RELAXED);
                    return;
                }
                continue;
            }
            continue;
        }
        Sleep(1u);
    }
}

void hyprlodr_file_hook_mark_redirects_ready(void) {
    LONG expected = HYPRLODR_ARCHIVE_GATE_PENDING;
    (void)__atomic_compare_exchange_n(&g_archive_gate_state, &expected,
                                      HYPRLODR_ARCHIVE_GATE_READY, 0,
                                      __ATOMIC_RELEASE, __ATOMIC_ACQUIRE);
    __atomic_store_n(&g_redirects_ready, 1, __ATOMIC_RELEASE);
    hyprlodr_startup_diag_emit(HYPRLODR_DIAG_GATE_RELEASE, 0u, 0u, 0u,
        (uint32_t)__atomic_load_n(&g_archive_gate_state, __ATOMIC_ACQUIRE), 0u,
        0u, 0u, "redirect registry published or bootstrap failed");
}

int hyprlodr_file_hook_begin_route_activation(void) {
    LONG expected = HYPRLODR_ARCHIVE_GATE_PENDING;
    return __atomic_compare_exchange_n(&g_archive_gate_state, &expected,
                                       HYPRLODR_ARCHIVE_GATE_ACTIVATING, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

int hyprlodr_file_hook_complete_route_activation(void) {
    LONG expected = HYPRLODR_ARCHIVE_GATE_ACTIVATING;
    if (__atomic_compare_exchange_n(&g_archive_gate_state, &expected,
                                    HYPRLODR_ARCHIVE_GATE_READY, 0,
                                    __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_redirects_ready, 1, __ATOMIC_RELEASE);
        hyprlodr_startup_diag_emit(HYPRLODR_DIAG_ROUTE_ACTIVATION, 0u, 0u, 0u,
            HYPRLODR_ARCHIVE_GATE_READY, 1u, 0u, 0u, "enabled");
        return 1;
    }
    hyprlodr_startup_diag_emit(HYPRLODR_DIAG_ROUTE_ACTIVATION, 0u, 0u, 0u,
        (uint32_t)expected, 0u, 0u, 0u, "activation aborted by archive fallback");
    return 0;
}

hyprlodr_archive_gate_state hyprlodr_file_hook_archive_gate_state(void) {
    return (hyprlodr_archive_gate_state)__atomic_load_n(&g_archive_gate_state,
                                                         __ATOMIC_ACQUIRE);
}

volatile long *hyprlodr_file_hook_archive_gate_word(void) {
    return &g_archive_gate_state;
}

/* Ask the published registry for a replacement path. Returns the path to open. */
static const char *resolve_path(const char *name, char *buffer, int *redirected) {
    if (is_asset_archive_path(name))
        hyprlodr_startup_diag_emit_once(HYPRLODR_DIAG_ARCHIVE_ATTEMPT, 0u, 0u, 0u,
            (uint32_t)hyprlodr_file_hook_archive_gate_state(), 0u, 0u, 0u, name);
    wait_for_asset_redirects(name);
    /* Once a bounded wait has fallen back, keep the archive identity pinned
     * for the rest of the process. A later bootstrap publication must not make
     * the game open the appended archive while record routing is disabled. */
    if (is_asset_archive_path(name) &&
        __atomic_load_n(&g_archive_gate_state, __ATOMIC_ACQUIRE) ==
            HYPRLODR_ARCHIVE_GATE_FALLBACK) {
        *redirected = 0;
        return name;
    }
    hyprlodr_loader *loader = __atomic_load_n(&g_loader, __ATOMIC_ACQUIRE);
    *redirected = 0;
    if (!name || !loader) return name;
    if (hyprlodr_loader_redirect_path(loader, name, buffer, HYPRLODR_REDIRECT_PATH_MAX)) {
        *redirected = 1;
        return buffer;
    }
    return name;
}

static HANDLE WINAPI intercept_create_file(LPCSTR name, DWORD access, DWORD share, void *security,
                                           DWORD disposition, DWORD flags, HANDLE template_file) {
    char replacement[HYPRLODR_REDIRECT_PATH_MAX];
    int redirected = 0;
    /* The saved pointer is written before the slot is patched and never changed
     * afterwards; read it once so a racing open sees a complete state. */
    create_file_fn original = g_original;
    if (!original) return INVALID_HANDLE_VALUE;
    const char *effective = resolve_path(name, replacement, &redirected);
    HANDLE handle =
        original(effective, access, share, security, disposition, flags, template_file);
    record_open(name, effective, "CreateFileA", redirected, handle != INVALID_HANDLE_VALUE);
    return handle;
}

static FILE *intercept_fopen(const char *name, const char *mode) {
    char replacement[HYPRLODR_REDIRECT_PATH_MAX];
    int redirected = 0;
    fopen_fn original = g_original_fopen;
    if (!original) return NULL;
    const char *effective = resolve_path(name, replacement, &redirected);
    FILE *file = original(effective, mode);
    record_open(name, effective, "fopen", redirected, file != NULL);
    return file;
}

/* The C++ stream entry point. This is the layer that sees the game's asset I/O:
 * std::ifstream calls _Fiopen inside MSVCP140, and that call goes on to the UCRT
 * by direct call, not through an import slot, so patching fopen/_wfopen slots can
 * never observe it. A real run held asset/asset open while every other target
 * logged zero opens, which is what pointed here. */
static FILE *intercept_fiopen(const char *name, int mode, int prot) {
    char replacement[HYPRLODR_REDIRECT_PATH_MAX];
    int redirected = 0;
    fiopen_fn original = g_original_fiopen;
    if (!original) return NULL;
    const char *effective = resolve_path(name, replacement, &redirected);
    FILE *file = original(effective, mode, prot);
    record_open(name, effective, "_Fiopen", redirected, file != NULL);
    return file;
}

/* UTF-16 path -> UTF-8, so the registry (which is narrow) can match it. */
static int wide_to_utf8(const wchar_t *wide, char *out, int capacity) {
    if (!wide) return 0;
    return WideCharToMultiByte(CP_UTF8, 0u, wide, -1, out, capacity, NULL, NULL);
}

static int utf8_to_wide(const char *narrow, wchar_t *out, int capacity) {
    if (!narrow) return 0;
    return MultiByteToWideChar(CP_UTF8, 0u, narrow, -1, out, capacity);
}

/* The wide CRT entry point: this is the one the game's asset loader appears to
 * use. A path that does not convert, or a replacement that does not convert
 * back, is passed through unchanged rather than guessed at. */
/* Shared wide-path resolution: returns the path the original should open and
 * reports whether that path is a replacement. */
static const wchar_t *resolve_wide(const wchar_t *name, char *narrow, size_t narrow_capacity,
                                   char *replacement, wchar_t *wide_replacement,
                                   size_t wide_capacity, int *redirected) {
    narrow[0] = '\0';
    *redirected = 0;
    if (wide_to_utf8(name, narrow, (int)narrow_capacity) <= 0) return name;
    resolve_path(narrow, replacement, redirected);
    if (!*redirected) return name;
    if (utf8_to_wide(replacement, wide_replacement, (int)wide_capacity) <= 0) {
        *redirected = 0;
        return name;
    }
    return wide_replacement;
}

static FILE *intercept_wfopen(const wchar_t *name, const wchar_t *mode) {
    char narrow[HYPRLODR_REDIRECT_PATH_MAX];
    char replacement[HYPRLODR_REDIRECT_PATH_MAX];
    wchar_t wide_replacement[HYPRLODR_REDIRECT_PATH_MAX];
    int redirected = 0;
    wfopen_fn original = g_original_wfopen;
    if (!original) return NULL;
    const wchar_t *effective = resolve_wide(name, narrow, sizeof narrow, replacement,
                                            wide_replacement,
                                            sizeof wide_replacement / sizeof wide_replacement[0],
                                            &redirected);
    FILE *file = original(effective, mode);
    if (narrow[0])
        record_open(narrow, redirected ? replacement : narrow, "_wfopen", redirected, file != NULL);
    return file;
}

static int intercept_wfopen_s(FILE **out, const wchar_t *name, const wchar_t *mode) {
    char narrow[HYPRLODR_REDIRECT_PATH_MAX];
    char replacement[HYPRLODR_REDIRECT_PATH_MAX];
    wchar_t wide_replacement[HYPRLODR_REDIRECT_PATH_MAX];
    int redirected = 0;
    wfopen_s_fn original = g_original_wfopen_s;
    if (!original) return 22; /* EINVAL */
    const wchar_t *effective = resolve_wide(name, narrow, sizeof narrow, replacement,
                                            wide_replacement,
                                            sizeof wide_replacement / sizeof wide_replacement[0],
                                            &redirected);
    int result = original(out, effective, mode);
    if (narrow[0])
        record_open(narrow, redirected ? replacement : narrow, "_wfopen_s", redirected,
                    result == 0);
    return result;
}

static int intercept_fopen_s(FILE **out, const char *name, const char *mode) {
    char replacement[HYPRLODR_REDIRECT_PATH_MAX];
    int redirected = 0;
    fopen_s_fn original = g_original_fopen_s;
    if (!original) return 22; /* EINVAL */
    const char *effective = resolve_path(name, replacement, &redirected);
    int result = original(out, effective, mode);
    record_open(name, effective, "fopen_s", redirected, result == 0);
    return result;
}

/* Find the IAT slot for `symbol` imported from the DLL whose name contains
 * `dll_filter`. Returns 1 when found, 0 when absent, -1 when the import table
 * cannot be trusted. */
static int find_import_slot(const pe_layout *pe, const char *dll_filter, const char *symbol,
                            void **slot) {
    const uint8_t *descriptor = rva_to_ptr(pe, pe->import_rva);
    if (!descriptor) return -1;

    for (unsigned index = 0; index < HYPRLODR_MAX_IMPORT_DESCRIPTORS; ++index) {
        const uint8_t *entry = descriptor + (size_t)index * HYPRLODR_IMPORT_DESCRIPTOR_BYTES;
        uint32_t name_rva = read_u32(entry + 12);
        uint32_t thunk_rva = read_u32(entry + 16);
        uint32_t names_rva = read_u32(entry);
        if (name_rva == 0) return 0;
        const char *dll = (const char *)rva_to_ptr(pe, name_rva);
        if (!dll || !contains_ignore_case(dll, dll_filter)) continue;
        if (thunk_rva == 0 || names_rva == 0) return -1;
        uint8_t *thunk = (uint8_t *)rva_to_ptr(pe, thunk_rva);
        const uint8_t *names = rva_to_ptr(pe, names_rva);
        if (!thunk || !names) return -1;
        /* FirstThunk already holds resolved addresses, so names come from
         * OriginalFirstThunk; both arrays share slot indices. */
        for (unsigned slot_index = 0; slot_index < HYPRLODR_MAX_IMPORT_SLOTS; ++slot_index) {
            uint8_t *item = thunk + (size_t)slot_index * 8u;
            uint64_t address = read_u64(item);
            uint64_t name_value = read_u64(names + (size_t)slot_index * 8u);
            if (address == 0 || name_value == 0) break;
            if ((name_value >> 63) != 0) continue; /* import by ordinal */
            const uint8_t *by_name = rva_to_ptr(pe, (uint32_t)(name_value & 0x7fffffffu));
            if (!by_name) continue;
            if (!equals_ignore_case((const char *)(by_name + 2), symbol)) continue;
            *slot = item;
            return 1;
        }
        return 0;
    }
    return 0;
}

/* Patch one slot, keeping the resolved original for the shim.
 * 1 = patched now, 2 = already ours, 0 = slot absent, -1 = cannot write.
 * The distinction keeps the per-target count honest across the two install
 * phases, and never overwrites a saved original with the shim. */
static int patch_slot(void *slot, void *replacement, void **original) {
    if (read_u64((const uint8_t *)slot) == (uint64_t)(uintptr_t)replacement) return 2;
    DWORD previous = 0;
    *original = (void *)(uintptr_t)read_u64((const uint8_t *)slot);
    /* The shim may run on another thread the instant the slot changes: publish
     * the saved original first, never after. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &previous)) return -1;
    memcpy(slot, &replacement, sizeof replacement);
    VirtualProtect(slot, sizeof(void *), previous, &previous);
    return 1;
}

typedef struct {
    const char *dll;
    const char *symbol;
    void *replacement;
    void **original;
    const char *label;
    int state; /* 1 patched, 0 absent, -1 failed */
    unsigned patched;
} hook_target;

static hook_target g_targets[] = {
    {"kernel32.dll", "CreateFileA", (void *)(uintptr_t)&intercept_create_file, (void **)&g_original,
     "CreateFileA", 0, 0},
    {"crt-stdio", "fopen", (void *)(uintptr_t)&intercept_fopen, (void **)&g_original_fopen, "fopen",
     0, 0},
    {"crt-stdio", "fopen_s", (void *)(uintptr_t)&intercept_fopen_s, (void **)&g_original_fopen_s,
     "fopen_s", 0, 0},
    {"crt-stdio", "_wfopen", (void *)(uintptr_t)&intercept_wfopen, (void **)&g_original_wfopen,
     "_wfopen", 0, 0},
    {"msvcp140.dll", "?_Fiopen@std@@YAPEAU_iobuf@@PEBDHH@Z", (void *)(uintptr_t)&intercept_fiopen,
     (void **)&g_original_fiopen, "_Fiopen", 0, 0},
    {"crt-stdio", "_wfopen_s", (void *)(uintptr_t)&intercept_wfopen_s,
     (void **)&g_original_wfopen_s, "_wfopen_s", 0, 0},
};

static const char *g_parse_error = "";

/* Read the PE headers of a loaded image. Every failure names itself, so an
 * unexpected module is skipped with a reason instead of being misread. */
static int prepare_layout(const uint8_t *base, pe_layout *layout) {
    g_parse_error = "no_image";
    if (!base || base[0] != 'M' || base[1] != 'Z') return -1;
    uint32_t pe_offset = read_u32(base + 0x3c);
    g_parse_error = "bad_dos_header";
    if (pe_offset < 0x40u || pe_offset > 0x1000u) return -1;
    const uint8_t *pe = base + pe_offset;
    g_parse_error = "bad_pe_header";
    if (!(pe[0] == 'P' && pe[1] == 'E' && pe[2] == 0 && pe[3] == 0)) return -1;
    const uint8_t *coff = pe + 4;
    const uint8_t *optional = coff + HYPRLODR_IMPORT_DESCRIPTOR_BYTES;
    g_parse_error = "not_pe32_plus";
    if (read_u16(optional) != 0x20bu) return -1;
    unsigned section_count = (unsigned)read_u16(coff + 2);
    uint16_t optional_size = read_u16(coff + 16);
    g_parse_error = "bad_optional_header";
    if (section_count == 0 || optional_size < 120u) return -1;
    /* PE32+ optional header: data directory 1 (imports) at offset 112 + 8. */
    uint32_t import_rva = read_u32(optional + 120);
    g_parse_error = "no_import_table";
    if (import_rva == 0) return -1;
    layout->base = base;
    layout->sections = optional + optional_size;
    layout->section_count = section_count;
    layout->import_rva = import_rva;
    g_parse_error = "";
    return 0;
}

static void patch_targets(const pe_layout *layout, hook_target *targets, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        void *slot = NULL;
        int found = find_import_slot(layout, targets[index].dll, targets[index].symbol, &slot);
        if (found < 0) {
            if (targets[index].state == 0) targets[index].state = -1;
            continue;
        }
        if (found == 0) continue;
        int result = patch_slot(slot, targets[index].replacement, targets[index].original);
        if (result > 0) {
            targets[index].state = 1;
            if (result == 1) ++targets[index].patched;
        } else {
            targets[index].state = -1;
        }
    }
}

/* Every loaded module keeps its own import table, and the game's C++ runtime
 * opens files through its own copy of these slots - not the executable's. The
 * proxy's own module is skipped, so its start-up file I/O is never redirected
 * or logged as game activity. */
static unsigned patch_loaded_modules(hook_target *targets, size_t count) {
    unsigned modules = 0;
    HANDLE snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    const uint8_t *self = (const uint8_t *)(uintptr_t)&g_targets;
    MODULEENTRY32A entry;
    memset(&entry, 0, sizeof entry);
    entry.dwSize = (DWORD)sizeof entry;
    for (BOOL more = Module32First(snapshot, &entry); more; more = Module32Next(snapshot, &entry)) {
        if (modules >= HYPRLODR_MAX_HOOK_MODULES) break;
        const uint8_t *module_base = (const uint8_t *)entry.modBaseAddr;
        if (module_base == self) continue;
        if (self >= module_base && self < module_base + entry.modBaseSize) continue;
        pe_layout layout;
        if (prepare_layout(module_base, &layout) != 0) continue;
        ++modules;
        patch_targets(&layout, targets, count);
    }
    CloseHandle(snapshot);
    return modules;
}

/* Phase 1, called from DllMain: patch only the main module's import slots. Do
 * no file I/O, module enumeration, or waiting while the loader lock is held.
 * The worker opens diagnostics and completes the broader module walk later. */
int hyprlodr_file_hook_install_early(hyprlodr_loader *loader, const char *log_path) {
    if (g_started) return g_installed ? 0 : -1;
    if (!log_path || !*log_path) return -1;
    g_started = 1;
    /* This first call is made from DllMain before the bootstrap thread exists. */
    g_loader = loader;
    __atomic_store_n(&g_start_tick, GetTickCount(), __ATOMIC_RELEASE);

    (void)log_path; /* The path is consumed by install() after loader-lock release. */

    pe_layout layout;
    if (prepare_layout((const uint8_t *)GetModuleHandleA(NULL), &layout) != 0) {
        g_status = "early_main_module_unparsed";
        return -1;
    }
    hook_target *targets = g_targets;
    const size_t target_count = sizeof g_targets / sizeof g_targets[0];
    patch_targets(&layout, targets, target_count);
    unsigned patched = 0;
    for (size_t index = 0; index < target_count; ++index)
        if (targets[index].patched) ++patched;
    g_installed = (int)patched;
    g_status = patched ? "installed_early_main_only" : "early_no_import_patched";
    hyprlodr_startup_diag_emit_once(HYPRLODR_DIAG_FILE_HOOK, 0u, 0u, 0u,
        (uint32_t)hyprlodr_file_hook_archive_gate_state(), patched ? 1u : 0u,
        0u, 0u, g_status);
    return patched ? 0 : -1;
}

int hyprlodr_file_hook_install(hyprlodr_loader *loader, const char *log_path) {
    if (g_complete) return 0;
    if (!loader || !log_path || !*log_path) return fail_install("invalid_argument");
    __atomic_store_n(&g_loader, loader, __ATOMIC_RELEASE);
    if (__atomic_load_n(&g_log, __ATOMIC_ACQUIRE) == INVALID_HANDLE_VALUE) {
        HANDLE log = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (log != INVALID_HANDLE_VALUE) {
            __atomic_store_n(&g_log, log, __ATOMIC_RELEASE);
            /* The diagnostic handle is opened only by the bootstrap worker,
             * after DllMain returned and the loader lock was released. */
            __atomic_store_n(&g_start_tick, GetTickCount(), __ATOMIC_RELEASE);
        }
    }
    /* DllMain may already have patched the main module; this is idempotent. */
    if (!g_started) (void)hyprlodr_file_hook_install_early(loader, log_path);
    __atomic_store_n(&g_loader, loader, __ATOMIC_RELEASE);

    pe_layout layout;
    if (prepare_layout((const uint8_t *)GetModuleHandleA(NULL), &layout) != 0)
        return fail_install(g_parse_error);

    hook_target *targets = g_targets;
    const size_t target_count = sizeof g_targets / sizeof g_targets[0];
    patch_targets(&layout, targets, target_count);
    unsigned modules = patch_loaded_modules(targets, target_count);
    g_installed = 0;
    for (size_t index = 0; index < target_count; ++index)
        if (targets[index].patched) ++g_installed;

    char detail[sizeof g_detail];
    size_t used = 0;
    for (size_t index = 0; index < target_count; ++index) {
        int written;
        if (targets[index].state == 1) {
            written = snprintf(detail + used, sizeof detail - used, "%s%s=patched(%u)",
                               index ? " " : "", targets[index].label, targets[index].patched);
        } else {
            written = snprintf(detail + used, sizeof detail - used, "%s%s=%s", index ? " " : "",
                               targets[index].label,
                               targets[index].state == 0 ? "absent" : "failed");
        }
        if (written > 0) used += (size_t)written;
        if (used >= sizeof detail) break;
    }
    if (used < sizeof detail) {
        int written = snprintf(detail + used, sizeof detail - used, " modules=%u", modules);
        if (written > 0) used += (size_t)written;
    }
    snprintf(g_detail, sizeof g_detail, "%s", detail);
    record_hook_install("all_modules", (unsigned)g_installed);

    if (g_installed == 0) return fail_install("no_import_patched");
    g_status = __atomic_load_n(&g_log, __ATOMIC_ACQUIRE) == INVALID_HANDLE_VALUE
                   ? "installed_no_log" : "installed";
    g_complete = 1;
    return 0;
}

const char *hyprlodr_file_hook_status(void) { return g_status; }

const char *hyprlodr_file_hook_detail(void) { return g_detail; }

int hyprlodr_file_hook_logged(void) {
    unsigned logged = __atomic_load_n(&g_logged, __ATOMIC_RELAXED);
    return (int)(logged > (unsigned)HYPRLODR_FILE_LOG_LIMIT ? (unsigned)HYPRLODR_FILE_LOG_LIMIT
                                                            : logged);
}

unsigned hyprlodr_file_hook_archive_waiters(void) {
    return __atomic_load_n(&g_asset_waiters, __ATOMIC_ACQUIRE);
}
