/*
 * HYPRLODR in-game bootstrap, built as the replacement OpenAL32.dll.
 *
 * hyperdemon.exe statically imports OpenAL32.dll, so the Windows loader resolves
 * this file from the game directory before any game code runs. Every OpenAL
 * export is a pure PE forwarder to OpenAL32_real.dll (see tools/gen_proxy_def.py
 * and tools/build_windows.sh), so audio behaviour is unchanged.
 *
 * Loader-lock safety: DLL_PROCESS_ATTACH only records paths and creates a worker
 * thread. All mod loading happens on that thread after the loader lock has been
 * released, so no mod code runs under it. Mods are never unloaded while the
 * process is alive; shutdown runs at DLL_PROCESS_DETACH with reserved == NULL
 * (explicit FreeLibrary), never during process termination.
 */
#define HYPRLODR_MINIMAL_WIN32 1

#include "hypr_win32.h"
#include "loader.h"
#include "file_hook.h"
#include "game_anchors.h"

#include <stdio.h>
#include <string.h>

#ifndef HYPRLODR_BOOTSTRAP_DELAY_MS
/* Let the game finish its own start-up before mods initialize. */
#  define HYPRLODR_BOOTSTRAP_DELAY_MS 2500
#endif

/* Upper bound for a configured delay, so a typo cannot park the thread forever. */
#define HYPRLODR_MAX_BOOTSTRAP_DELAY_MS 60000

static hyprlodr_loader *g_loader;
static DWORD g_delay_ms = HYPRLODR_BOOTSTRAP_DELAY_MS;
static char g_game_dir[1024];
static char g_evidence_path[1024];

static void trim_filename(char *path) {
    size_t length = strlen(path);
    while (length > 0 && path[length - 1] != '\\' && path[length - 1] != '/') --length;
    if (length > 0) path[length - 1] = '\0';
}

/* Windows paths contain backslashes; emit valid JSON strings. */
static void json_escape(FILE *file, const char *text) {
    for (const char *cursor = text ? text : ""; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '"') fputc('\\', file);
        fputc(*cursor, file);
    }
}

/* --- build fingerprint --------------------------------------------------------
 * The proxy is the only HYPRLODR component that runs inside the game, so it is
 * the only place that can prove which executable is actually running. It
 * verifies the anchor table from native/win32/game_anchors.h against the mapped
 * main module and reports the result in the attach record.
 *
 * Nothing is hooked today, so a mismatch is reported rather than fatal. Any
 * address-dependent work must require build_matches. Nothing in the game is
 * assumed to be where the analysis said it was without this proof.
 */
typedef struct {
    const uint8_t *base;
    uint32_t size_of_image;
    uint32_t timestamp;
    int parsed;
} module_identity;

static module_identity g_module;
static hyprlodr_anchor_report g_anchors;
static int g_anchors_checked;
static int g_tables_dumped;

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

static void inspect_main_module(void) {
    g_anchors_checked = 1;
    HMODULE handle = GetModuleHandleA(NULL);
    if (!handle) return;
    const uint8_t *base = (const uint8_t *)handle;
    if (base[0] != 'M' || base[1] != 'Z') return;
    uint32_t pe_offset = read_u32(base + 0x3c);
    if (pe_offset < 0x40 || pe_offset > 0x1000) return;
    const uint8_t *pe = base + pe_offset;
    if (!(pe[0] == 'P' && pe[1] == 'E' && pe[2] == 0 && pe[3] == 0)) return;
    const uint8_t *coff = pe + 4;
    const uint8_t *optional = coff + 20;
    if (read_u16(optional) != 0x20b) return; /* PE32+ only */
    g_module.timestamp = read_u32(coff + 4);
    g_module.size_of_image = read_u32(optional + 56);
    if (g_module.size_of_image < 0x1000) return;
    g_module.base = base;
    g_module.parsed = 1;
    hyprlodr_anchors_verify(base, (size_t)g_module.size_of_image, hyprlodr_game_anchors,
                            HYPRLODR_GAME_ANCHOR_COUNT, &g_anchors);
}

/* Copy the asset cipher's tables out of the running image.
 *
 * The loader's own code indexes two 256-byte tables. Offline work could not
 * reconcile the file's .rdata copy with the cipher, so capture what the process
 * actually has: if the runtime bytes differ from the file, the tables are built
 * at start-up and the offline copy is the wrong input. Written as two
 * consecutive 256-byte blocks. */
static void dump_asset_tables(void) {
    if (g_tables_dumped || !g_module.parsed) return;
    g_tables_dumped = 1;
    uint32_t last = HYPRLODR_GAME_TABLE_T1_RVA + HYPRLODR_GAME_TABLE_BYTES;
    if (last > g_module.size_of_image) return; /* not this binary */

    char path[1024];
    snprintf(path, sizeof path, "%s\\hyprlodr-tables.bin", g_game_dir);
    FILE *file = fopen(path, "wb");
    if (!file) return;
    for (uint32_t offset = 0; offset < HYPRLODR_GAME_TABLE_BYTES; ++offset)
        fputc((int)g_module.base[HYPRLODR_GAME_TABLE_T0_RVA + offset], file);
    for (uint32_t offset = 0; offset < HYPRLODR_GAME_TABLE_BYTES; ++offset)
        fputc((int)g_module.base[HYPRLODR_GAME_TABLE_T1_RVA + offset], file);
    fclose(file);
}

static void put_hex(FILE *file, uint64_t value, unsigned digits) {
    static const char alphabet[] = "0123456789abcdef";
    fputs("0x", file);
    for (unsigned shift = (digits - 1u) * 4u;; shift -= 4u) {
        fputc(alphabet[(value >> shift) & 0xfu], file);
        if (shift == 0) break;
    }
}

/* --- crash diagnostics --------------------------------------------------------
 * The game can die during start-up without recording anything, leaving Steam's
 * crash handler with a dump that names no module. This filter only runs when an
 * exception would otherwise be unhandled, writes the code and faulting address,
 * and then always hands control back to whatever filter was installed before us
 * (Steam installs one), so crash reporting behaviour is unchanged.
 */
static LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter;

static LONG WINAPI hyprlodr_crash_filter(HYPR_EXCEPTION_POINTERS *info) {
    uint32_t code = 0;
    uint64_t address = 0;
    if (info && info->ExceptionRecord) {
        code = info->ExceptionRecord->ExceptionCode;
        address = (uint64_t)(uintptr_t)info->ExceptionRecord->ExceptionAddress;
    }

    char path[1024];
    snprintf(path, sizeof path, "%s\\hyprlodr-crash.json", g_game_dir);
    FILE *file = fopen(path, "w");
    if (file) {
        fputs("{\"exception_code\":\"", file);
        put_hex(file, (uint64_t)code, 8u);
        fputs("\",\"exception_address\":\"", file);
        put_hex(file, address, 16u);
        fputs("\",\"module_base\":\"", file);
        put_hex(file, (uint64_t)(uintptr_t)g_module.base, 16u);
        fputs("\"", file);
        if (g_module.parsed && address >= (uint64_t)(uintptr_t)g_module.base) {
            fputs(",\"module_offset\":\"", file);
            put_hex(file, address - (uint64_t)(uintptr_t)g_module.base, 8u);
            fputs("\"", file);
        }
        fputs("}\n", file);
        fclose(file);
    }

    /* 0 == EXCEPTION_CONTINUE_SEARCH: let the previous handler decide. */
    return g_previous_filter ? g_previous_filter(info) : 0;
}

/* --- start-up delay configuration -------------------------------------------
 * <game>\hyprlodr-config.json may override the mod start-up delay:
 *
 *     { "delay_ms": 0 }
 *
 * A machine that reaches its first frame faster than the built-in 2500 ms is
 * common, and a process that exits before the delay elapses never reports mod
 * initialization at all. The parser is deliberately tiny (no allocator, no
 * locale, line oriented) and any problem falls back to the default instead of
 * failing to attach: a malformed config must never stop mods from loading.
 */
static const char *find_literal(const char *text, const char *needle) {
    size_t length = strlen(needle);
    if (length == 0) return text;
    for (; *text; ++text) {
        if (*text == *needle && strncmp(text, needle, length) == 0) return text;
    }
    return NULL;
}

static DWORD bootstrap_delay_ms(void) {
    char path[1024];
    snprintf(path, sizeof path, "%s\\hyprlodr-config.json", g_game_dir);
    FILE *file = fopen(path, "rb");
    if (!file) return (DWORD)HYPRLODR_BOOTSTRAP_DELAY_MS;

    DWORD result = (DWORD)HYPRLODR_BOOTSTRAP_DELAY_MS;
    char line[512];
    while (fgets(line, (int)sizeof line, file)) {
        const char *key = find_literal(line, "\"delay_ms\"");
        if (!key) continue;
        const char *cursor = key + strlen("\"delay_ms\"");
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') ++cursor;
        if (*cursor != ':') continue;
        ++cursor;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor < '0' || *cursor > '9') continue;
        unsigned long value = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            value = value * 10ul + (unsigned long)(*cursor - '0');
            if (value > HYPRLODR_MAX_BOOTSTRAP_DELAY_MS) {
                value = HYPRLODR_MAX_BOOTSTRAP_DELAY_MS;
                break;
            }
            ++cursor;
        }
        result = (DWORD)value;
        break;
    }
    fclose(file);
    return result;
}

static void write_attach_record(const char *stage, int status) {
    if (!g_anchors_checked) inspect_main_module();
    FILE *file = fopen(g_evidence_path, "w");
    if (!file) return;
    char exe_path[1024];
    if (GetModuleFileNameA(NULL, exe_path, (DWORD)sizeof exe_path) == 0) exe_path[0] = '\0';
    fputs("{\"stage\":\"", file);
    json_escape(file, stage);
    fprintf(file, "\",\"pid\":%lu,\"proxy_dir\":\"", (unsigned long)GetCurrentProcessId());
    json_escape(file, g_game_dir);
    fputs("\",\"exe\":\"", file);
    json_escape(file, exe_path);
    fprintf(file, "\",\"loader_abi\":%u,\"status\":%d,\"status_text\":\"",
            (unsigned)HYPRLODR_ABI_VERSION, status);
    json_escape(file, hyprlodr_loader_strerror(status));

    fprintf(file, "\",\"delay_ms\":%lu,\"module_parsed\":", (unsigned long)g_delay_ms);
    fputs(g_module.parsed ? "true" : "false", file);
    fputs(",\"module_base\":\"", file);
    put_hex(file, (uint64_t)(uintptr_t)g_module.base, 16u);
    fprintf(file, "\",\"module_size\":%lu", (unsigned long)g_module.size_of_image);
    fputs(",\"build_timestamp\":\"", file);
    put_hex(file, (uint64_t)g_module.timestamp, 8u);
    fputs("\",\"expected_timestamp\":\"", file);
    put_hex(file, (uint64_t)HYPRLODR_GAME_BUILD_TIMESTAMP, 8u);

    int build_matches = g_module.parsed != 0 &&
                        g_module.timestamp == HYPRLODR_GAME_BUILD_TIMESTAMP &&
                        g_anchors.total > 0 && g_anchors.matched == g_anchors.total;
    fputs("\",\"build_matches\":", file);
    fputs(build_matches ? "true" : "false", file);
    fprintf(file, ",\"anchors_total\":%lu,\"anchors_matched\":%lu,\"anchors_out_of_range\":%lu",
            (unsigned long)g_anchors.total, (unsigned long)g_anchors.matched,
            (unsigned long)g_anchors.out_of_range);
    fputs(",\"anchor_mismatch\":\"", file);
    json_escape(file, g_anchors.first_mismatch ? g_anchors.first_mismatch : "");

    dump_asset_tables();
    fputs("\",\"tables_dumped\":", file);
    fputs(g_tables_dumped && g_module.parsed &&
                  HYPRLODR_GAME_TABLE_T1_RVA + HYPRLODR_GAME_TABLE_BYTES <= g_module.size_of_image
              ? "true"
              : "false",
          file);
    fputs(",\"table_t0_head\":\"", file);
    if (g_module.parsed &&
        HYPRLODR_GAME_TABLE_T1_RVA + HYPRLODR_GAME_TABLE_BYTES <= g_module.size_of_image) {
        for (unsigned index = 0; index < 8u; ++index) {
            fputc("0123456789abcdef"[(g_module.base[HYPRLODR_GAME_TABLE_T0_RVA + index] >> 4) & 0xf], file);
            fputc("0123456789abcdef"[g_module.base[HYPRLODR_GAME_TABLE_T0_RVA + index] & 0xf], file);
        }
    }
    fputs("\",\"file_hook\":\"", file);
    json_escape(file, hyprlodr_file_hook_status());
    fputs("\",\"file_hook_detail\":\"", file);
    json_escape(file, hyprlodr_file_hook_detail());
    fprintf(file, "\",\"file_redirects\":%lu,\"file_opens_logged\":%d}\n",
            (unsigned long)hyprlodr_loader_redirect_count(g_loader),
            hyprlodr_file_hook_logged());
    fclose(file);
}

static DWORD WINAPI bootstrap(LPVOID unused) {
    (void)unused;

    /* Evidence and build identity first. A game that exits during start-up can
     * still prove which executable ran and whether it matches the analysis; the
     * mod start-up delay below is about the game being ready, not about us. */
    g_delay_ms = bootstrap_delay_ms();

    char mods_dir[1024];
    char disabled_file[1024];
    char log_path[1024];
    char state_root[1024];
    char files_log[1024];
    snprintf(mods_dir, sizeof mods_dir, "%s\\mods", g_game_dir);
    snprintf(disabled_file, sizeof disabled_file, "%s\\mods\\disabled.txt", g_game_dir);
    snprintf(log_path, sizeof log_path, "%s\\hyprlodr-events.jsonl", g_game_dir);
    snprintf(state_root, sizeof state_root, "%s\\hyprlodr-state", g_game_dir);
    snprintf(files_log, sizeof files_log, "%s\\hyprlodr-files.jsonl", g_game_dir);

    g_loader = hyprlodr_loader_create();
    if (!g_loader) {
        write_attach_record("no_memory", HYPRLODR_ERR_LIMIT);
        return 0;
    }
    if (hyprlodr_loader_set_log(g_loader, log_path) != HYPRLODR_OK) {
        write_attach_record("log_open_failed", HYPRLODR_ERR_IO);
        hyprlodr_loader_destroy(g_loader);
        g_loader = NULL;
        return 0;
    }
    hyprlodr_loader_add_dir(g_loader, mods_dir);
    hyprlodr_loader_disable_file(g_loader, disabled_file);
    /* Per-mod writable directories live beside the loader's own files. */
    hyprlodr_loader_set_state_root(g_loader, state_root);

    /* Intercept the game's own CreateFileA calls before the delay, so opens the
     * game performs while mods are still starting are observed too. Nothing is
     * patched unless the import table really contains that entry. */
    hyprlodr_file_hook_install(g_loader, files_log);

    write_attach_record("attached", HYPRLODR_OK);

    if (g_delay_ms) Sleep(g_delay_ms);

    /* Rewriting the record with this stage proves the process outlived the
     * delay; a record still reading "attached" means it did not. */
    write_attach_record("initializing", HYPRLODR_OK);

    int status = hyprlodr_loader_resolve(g_loader);
    if (status == HYPRLODR_OK) status = hyprlodr_loader_init(g_loader);
    write_attach_record(status == HYPRLODR_OK ? "mods_loaded" : "mods_failed", status);
    return 0;
}

BOOL WINAPI dll_main(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        char module_path[1024];
        module_path[0] = '\0';
        GetModuleFileNameA((HMODULE)instance, module_path, (DWORD)sizeof module_path);
        snprintf(g_game_dir, sizeof g_game_dir, "%s", module_path);
        trim_filename(g_game_dir);
        snprintf(g_evidence_path, sizeof g_evidence_path, "%s\\hyprlodr-attach.json", g_game_dir);
        DisableThreadLibraryCalls((HMODULE)instance);
        /* Chain, do not replace: Steam's crash handler stays in charge. */
        g_previous_filter = SetUnhandledExceptionFilter(hyprlodr_crash_filter);
        /* Never run mod code or file I/O for mods under the loader lock. */
        CreateThread(NULL, 0, bootstrap, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH && reserved == NULL) {
        if (g_loader) {
            hyprlodr_loader_shutdown(g_loader);
            hyprlodr_loader_destroy(g_loader);
            g_loader = NULL;
        }
    }
    return TRUE;
}
