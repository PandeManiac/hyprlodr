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
 *   api-ms-win-crt-stdio-l1-1-0.dll  fopen        (the asset-loading path)
 *   api-ms-win-crt-stdio-l1-1-0.dll  fopen_s
 *
 * Measured gap that motivated the CRT targets: a real run patched CreateFileA
 * and observed zero opens while five mods initialized - the game's asset I/O
 * never passes through that import. hyperdemon.exe imports fopen/fopen_s and
 * _wfopen from the UCRT API set, and only CreateFileA from KERNEL32, so the CRT
 * slots are the ones the game actually uses. _wfopen is not hooked: it takes
 * wide strings and the redirect registry is UTF-8/narrow.
 *
 * Failure policy: a target that is absent, malformed or unwritable is reported
 * and skipped; the remaining targets still install and every mod still starts.
 * Nothing is patched unless the PE structures actually describe the slot.
 */
#define HYPRLODR_MINIMAL_WIN32 1

#include "hypr_win32.h"
#include "file_hook.h"

#include <stdio.h>
#include <string.h>

#define HYPRLODR_IMPORT_DESCRIPTOR_BYTES 20u
#define HYPRLODR_SECTION_HEADER_BYTES 40u
#define HYPRLODR_MAX_IMPORT_DESCRIPTORS 256u
#define HYPRLODR_MAX_IMPORT_SLOTS 4096u
/* An unbounded log would let a streaming game fill the disk. */
#define HYPRLODR_FILE_LOG_LIMIT 4096

typedef HANDLE(WINAPI *create_file_fn)(LPCSTR name, DWORD access, DWORD share, void *security,
                                       DWORD disposition, DWORD flags, HANDLE template_file);
/* MSVC x64 CRT entry points use the platform calling convention; these are
 * passed through untouched, so the exact typedef only has to match the ABI. */
typedef FILE *(*fopen_fn)(const char *name, const char *mode);
typedef int (*fopen_s_fn)(FILE **out, const char *name, const char *mode);

typedef struct {
    const uint8_t *base;
    const uint8_t *sections;
    unsigned section_count;
    uint32_t import_rva;
} pe_layout;

static create_file_fn g_original;
static fopen_fn g_original_fopen;
static fopen_s_fn g_original_fopen_s;
static hyprlodr_loader *g_loader;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static unsigned g_logged;
static int g_installed;
static const char *g_status = "not_attempted";
/* Which targets were patched, so a host reports exactly what is intercepted. */
static char g_detail[192] = "";

/* A failed install must not leave a log handle open, or a later retry would
 * append to a file nobody owns. */
static int fail_install(const char *status) {
    g_status = status;
    if (g_log != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
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

static void record_open(const char *requested, const char *opened, const char *via, int redirected,
                        int ok) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    char requested_json[768];
    char opened_json[768];
    char line[2048];
    DWORD written = 0;
    /* Game threads open files concurrently. Reserve the sequence number
     * atomically so two records can never claim the same one, and stop writing
     * once the cap is reached. */
    unsigned sequence = __atomic_add_fetch(&g_logged, 1u, __ATOMIC_RELAXED);
    if (sequence > (unsigned)HYPRLODR_FILE_LOG_LIMIT) return;
    escape_into(requested_json, sizeof requested_json, requested);
    escape_into(opened_json, sizeof opened_json, opened);
    snprintf(line, sizeof line,
             "{\"seq\":%u,\"event\":\"file_open\",\"via\":\"%s\",\"redirected\":%s,\"ok\":%s,"
             "\"requested\":\"%s\",\"opened\":\"%s\"}\n",
             sequence, via, redirected ? "true" : "false", ok ? "true" : "false", requested_json,
             opened_json);
    /* The handle is opened for append, so each WriteFile lands at end-of-file
     * without a lock: game threads may interleave records but never corrupt the
     * file pointer, and no kernel32 interlocked import is needed. */
    WriteFile(g_log, line, (DWORD)strlen(line), &written, NULL);
}

/* Ask the published registry for a replacement path. Returns the path to open. */
static const char *resolve_path(const char *name, char *buffer, int *redirected) {
    hyprlodr_loader *loader = g_loader;
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

/* Patch one slot, keeping the resolved original for the shim. Returns 1 on
 * success, 0 when the slot is absent, -1 when it cannot be written. */
static int patch_slot(void *slot, void *replacement, void **original) {
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

int hyprlodr_file_hook_install(hyprlodr_loader *loader, const char *log_path) {
    if (g_installed) return 0;
    if (!loader || !log_path || !*log_path) return fail_install("invalid_argument");
    g_loader = loader;

    /* Open the log before patching, so this open cannot re-enter a shim. */
    g_log = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    const uint8_t *base = (const uint8_t *)GetModuleHandleA(NULL);
    if (!base || base[0] != 'M' || base[1] != 'Z') return fail_install("no_image");
    uint32_t pe_offset = read_u32(base + 0x3c);
    if (pe_offset < 0x40u || pe_offset > 0x1000u) return fail_install("bad_dos_header");
    const uint8_t *pe = base + pe_offset;
    if (!(pe[0] == 'P' && pe[1] == 'E' && pe[2] == 0 && pe[3] == 0))
        return fail_install("bad_pe_header");
    const uint8_t *coff = pe + 4;
    const uint8_t *optional = coff + HYPRLODR_IMPORT_DESCRIPTOR_BYTES;
    if (read_u16(optional) != 0x20bu) return fail_install("not_pe32_plus");
    unsigned section_count = (unsigned)read_u16(coff + 2);
    uint16_t optional_size = read_u16(coff + 16);
    if (section_count == 0 || optional_size < 120u) return fail_install("bad_optional_header");

    /* PE32+ optional header: data directory 1 (imports) at offset 112 + 8. */
    uint32_t import_rva = read_u32(optional + 120);
    if (import_rva == 0) return fail_install("no_import_table");

    pe_layout layout;
    layout.base = base;
    layout.sections = optional + optional_size;
    layout.section_count = section_count;
    layout.import_rva = import_rva;

    struct target {
        const char *dll;
        const char *symbol;
        void *replacement;
        void **original;
        const char *label;
        int state; /* 1 patched, 0 absent, -1 failed */
    } targets[] = {
        {"kernel32.dll", "CreateFileA", (void *)(uintptr_t)&intercept_create_file,
         (void **)&g_original, "CreateFileA", 0},
        {"crt-stdio", "fopen", (void *)(uintptr_t)&intercept_fopen,
         (void **)&g_original_fopen, "fopen", 0},
        {"crt-stdio", "fopen_s", (void *)(uintptr_t)&intercept_fopen_s,
         (void **)&g_original_fopen_s, "fopen_s", 0},
    };
    const size_t target_count = sizeof targets / sizeof targets[0];

    for (size_t index = 0; index < target_count; ++index) {
        void *slot = NULL;
        int found = find_import_slot(&layout, targets[index].dll, targets[index].symbol, &slot);
        if (found <= 0) {
            targets[index].state = found < 0 ? -1 : 0;
            continue;
        }
        targets[index].state = patch_slot(slot, targets[index].replacement, targets[index].original);
        if (targets[index].state == 1) ++g_installed;
    }

    char detail[sizeof g_detail];
    size_t used = 0;
    for (size_t index = 0; index < target_count; ++index) {
        const char *state = targets[index].state == 1   ? "patched"
                            : targets[index].state == 0 ? "absent"
                                                        : "failed";
        int written = snprintf(detail + used, sizeof detail - used, "%s%s=%s",
                               index ? " " : "", targets[index].label, state);
        if (written > 0) used += (size_t)written;
        if (used >= sizeof detail) break;
    }
    snprintf(g_detail, sizeof g_detail, "%s", detail);

    if (g_installed == 0) return fail_install("no_import_patched");
    g_status = g_log == INVALID_HANDLE_VALUE ? "installed_no_log" : "installed";
    return 0;
}

const char *hyprlodr_file_hook_status(void) { return g_status; }

const char *hyprlodr_file_hook_detail(void) { return g_detail; }

int hyprlodr_file_hook_logged(void) {
    unsigned logged = __atomic_load_n(&g_logged, __ATOMIC_RELAXED);
    return (int)(logged > (unsigned)HYPRLODR_FILE_LOG_LIMIT ? (unsigned)HYPRLODR_FILE_LOG_LIMIT
                                                            : logged);
}
