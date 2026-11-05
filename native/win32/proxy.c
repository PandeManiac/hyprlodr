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
#include "proxy_build.h"
#include "gl_hook.h"
#include "game_anchors.h"
#include "route_manifest.h"
#include "callsite_hook.h"
#include "record_route_hook.h"
#include "startup_diag.h"

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
static int g_record_route_requested;
static char g_record_route_status[64] = "disabled";
static int g_early_hook_requested;
static int g_startup_diag_requested;
static int g_diag_writer_start_result = -1;
static char g_early_hook_status[64] = "not_requested";
static hyprlodr_manifest_result g_route_manifest;
static hyprlodr_call_hook g_route_call_hook = HYPRLODR_CALL_HOOK_INITIALIZER;
static hyprlodr_game_route_binding g_route_binding;
static char g_route_archive_path[1024];
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
 * The experimental record-call hook is installed only during an explicitly
 * opted-in process-start attach after these mapped-image anchors and call bytes
 * match. Worker-side manifest validation separately checks the full file hash.
 * Address-dependent work requires its relevant build proof.
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

static uint64_t read_u64(const uint8_t *bytes) {
    uint64_t value;
    memcpy(&value, bytes, sizeof value);
    return value;
}

static int bytes_equal(const unsigned char *left, const unsigned char *right,
                       size_t length) {
    for (size_t index = 0; index < length; ++index)
        if (left[index] != right[index]) return 0;
    return 1;
}

static void inspect_main_module(void) {
    /* Keep the attach-time fingerprint.  The early record hook deliberately
     * replaces the verified CALL instruction, so verifying this image again
     * from the bootstrap worker would report that one expected patch as an
     * unsupported executable and disable routing.  An invalid initial image
     * remains fail-closed; it is never upgraded by a later inspection. */
    if (g_anchors_checked) return;
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

/* Early patching is accepted only when the executable proves the proxy is a
 * static dependency and the executable itself has no TLS callback that could
 * have run before the imported DLL's process attach. This bounded PE read uses
 * the already mapped image and performs no allocation or file I/O. */
static int startup_import_layout_supported(void) {
    const uint8_t *base = g_module.base;
    if (!base || g_module.size_of_image < 0x1000u) return 0;
    uint32_t pe_offset = read_u32(base + 0x3cu);
    if (pe_offset > g_module.size_of_image - 24u) return 0;
    const uint8_t *pe = base + pe_offset;
    if (pe[0] != 'P' || pe[1] != 'E' || pe[2] != 0 || pe[3] != 0) return 0;
    const uint8_t *coff = pe + 4u;
    const uint8_t *optional = coff + 20u;
    uint16_t optional_size = read_u16(coff + 16u);
    if (read_u16(optional) != 0x20bu || optional_size < 192u) return 0;
    uint32_t directory_count = read_u32(optional + 108u);
    if (directory_count <= 9u) return 0;

    uint32_t import_rva = read_u32(optional + 120u);
    uint32_t import_size = read_u32(optional + 124u);
    if (!import_rva || import_size < 20u || import_rva > g_module.size_of_image ||
        import_size > g_module.size_of_image - import_rva) return 0;
    int found_openal = 0;
    uint32_t descriptor_count = import_size / 20u;
    if (descriptor_count > 2048u) descriptor_count = 2048u;
    for (uint32_t index = 0; index < descriptor_count; ++index) {
        const uint8_t *descriptor = base + import_rva + (size_t)index * 20u;
        uint32_t name_rva = read_u32(descriptor + 12u);
        if (!name_rva) break;
        if (name_rva >= g_module.size_of_image) return 0;
        const char *name = (const char *)(base + name_rva);
        static const char expected[] = "openal32.dll";
        size_t cursor = 0;
        while (cursor < sizeof expected && name_rva + cursor < g_module.size_of_image) {
            char actual = name[cursor];
            if (actual >= 'A' && actual <= 'Z') actual = (char)(actual - 'A' + 'a');
            if (actual != expected[cursor]) break;
            if (actual == '\0') { found_openal = 1; break; }
            ++cursor;
        }
        if (found_openal) break;
        /* Reject unterminated or unreadable import names instead of guessing. */
        size_t length = 0;
        while (length < 260u && name_rva + length < g_module.size_of_image &&
               name[length] != '\0') ++length;
        if (length == 260u || name_rva + length >= g_module.size_of_image) return 0;
    }
    if (!found_openal) return 0;

    uint32_t tls_rva = read_u32(optional + 184u);
    uint32_t tls_size = read_u32(optional + 188u);
    if (!tls_rva && !tls_size) return 1;
    if (!tls_rva || tls_size < 40u || tls_rva > g_module.size_of_image ||
        40u > g_module.size_of_image - tls_rva) return 0;
    uint64_t callbacks_va = read_u64(base + tls_rva + 24u);
    if (!callbacks_va) return 1;
    uintptr_t image_base = (uintptr_t)base;
    if (callbacks_va < image_base || callbacks_va - image_base > g_module.size_of_image - 8u)
        return 0;
    return read_u64(base + (size_t)(callbacks_va - image_base)) == 0u;
}

/* Page protection is the host's business: the image's .text and .rdata are
 * read-only, so a patch is applied between two VirtualProtect calls. */
static int image_write_bytes(void *context, void *destination, const void *source, size_t length) {
    (void)context;
    if (!destination || !source || length == 0) return -1;
    DWORD previous = 0;
    if (!VirtualProtect(destination, length, PAGE_EXECUTE_READWRITE, &previous)) return -1;
    memcpy(destination, source, length);
    DWORD ignored = 0;
    VirtualProtect(destination, length, previous, &ignored);
    return 0;
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

static int bootstrap_record_route_requested(void) {
    char path[1024];
    snprintf(path, sizeof path, "%s\\hyprlodr-config.json", g_game_dir);
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    int requested = 0;
    char line[512];
    while (fgets(line, (int)sizeof line, file)) {
        const char *key = find_literal(line, "\"experimental_record_route\"");
        if (!key) continue;
        const char *cursor = key + strlen("\"experimental_record_route\"");
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') ++cursor;
        if (*cursor++ != ':') continue;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (strncmp(cursor, "true", 4u) == 0) requested = 1;
        else if (strncmp(cursor, "false", 5u) == 0) requested = 0;
        break;
    }
    fclose(file);
    return requested;
}

static int startup_diag_requested(void) {
    char value[8];
    DWORD length = GetEnvironmentVariableA("HYPRLODR_FIRST_LOAD_DIAGNOSTICS",
                                           value, (DWORD)sizeof value);
    return length == 1u && value[0] == '1';
}

/* This switch is intentionally separate from the ordinary config. It opts in
 * only to a dormant pre-entry hook; the worker config and validated manifest
 * remain authoritative for route activation. */
static int early_record_route_requested(void) {
    char value[8];
    DWORD length = GetEnvironmentVariableA("HYPRLODR_EARLY_RECORD_HOOK", value,
                                            (DWORD)sizeof value);
    return length == 1u && value[0] == '1' && value[1] == '\0';
}

static int config_string(const char *key, char *out, size_t capacity) {
    char path[1024], line[1024];
    snprintf(path, sizeof path, "%s\\hyprlodr-config.json", g_game_dir);
    FILE *file=fopen(path,"rb"); if(!file)return 0;
    char needle[128]; snprintf(needle,sizeof needle,"\"%s\"",key);
    int found=0;
    while(fgets(line,(int)sizeof line,file)) {
        const char *at=find_literal(line,needle); if(!at)continue;
        at+=strlen(needle); while(*at==' '||*at=='\t')++at;
        if(*at++!=':')break; while(*at==' '||*at=='\t')++at;
        if(*at++!='\"')break; size_t n=0;
        while(*at&&*at!='\"'&&(unsigned char)*at>=0x20) {
            if(*at=='\\') { if(at[1]!='\\'&&at[1]!='\"')break; ++at; }
            if(n+1>=capacity)break; out[n++]=*at++;
        }
        if(*at!='\"'||n+1>capacity)break;
        out[n]=0;found=1;break;
    }
    fclose(file); return found;
}

static int safe_route_path(const char *path) {
    const char *component=path;
    if(!path||!*path)return 0;
    for(const char*p=path;;++p) {
        unsigned char c=(unsigned char)*p;
        if(c=='*'||c=='?'||c=='<'||c=='>'||c=='|'||c=='"'||c<0x20)return 0;
        if(c=='\\'||c=='/'||c==0) {
            if((size_t)(p-component)==2&&component[0]=='.'&&component[1]=='.')return 0;
            if(c==0)break; component=p+1;
        }
    }
    return 1;
}

static void preflight_record_route(int build_matches) {
    if(!g_record_route_requested){snprintf(g_record_route_status,sizeof g_record_route_status,"disabled");return;}
    if(!build_matches){snprintf(g_record_route_status,sizeof g_record_route_status,"executable_unsupported");return;}
    char manifest[1024],archive[1024],exe[1024];
    if(!config_string("experimental_record_route_manifest",manifest,sizeof manifest)) {
        snprintf(g_record_route_status,sizeof g_record_route_status,"manifest_missing");return;
    }
    if(!safe_route_path(manifest)) { snprintf(g_record_route_status,sizeof g_record_route_status,"manifest_malformed");return; }
    if(!config_string("experimental_record_route_archive",archive,sizeof archive)) {
        snprintf(g_record_route_status,sizeof g_record_route_status,"archive_mismatch");return;
    }
    if(!safe_route_path(archive)) { snprintf(g_record_route_status,sizeof g_record_route_status,"archive_mismatch");return; }
    snprintf(g_route_archive_path, sizeof g_route_archive_path, "%s", archive);
    if(!GetModuleFileNameA(NULL,exe,(DWORD)sizeof exe)) {
        snprintf(g_record_route_status,sizeof g_record_route_status,"executable_unsupported");return;
    }
    hyprlodr_manifest_inputs input={exe,archive,manifest,
        "7612dd72076002b7c0cb5cab8f7340a399176ecedefe9d3b4d0353fbdac93611",
        "tbl-e91fb3b80004","rec-011a0ebc0-000055d4-0001",295758784u,21972u,1u};
    hyprlodr_manifest_status status=hyprlodr_manifest_validate(&input,&g_route_manifest);
    if(status!=HYPRLODR_MANIFEST_OK){snprintf(g_record_route_status,sizeof g_record_route_status,"%s",hyprlodr_manifest_status_name(status));return;}
    /* Route storage is immutable and process-lived. The early binding is
     * already reachable in dormant mode; publish only the route pointer here. */
    hyprlodr_record_route_hook_publish_route(&g_route_binding, &g_route_manifest.route);
    if (!g_route_call_hook.installed) {
        snprintf(g_record_route_status, sizeof g_record_route_status,
                 "validated_early_hook_missing");
        return;
    }
    snprintf(g_record_route_status,sizeof g_record_route_status,"manifest_validated_waiting_for_redirect");
}

static const char *call_hook_status_text(hyprlodr_call_hook_status status) {
    switch (status) {
        case HYPRLODR_CALL_HOOK_OK: return "installed_dormant";
        case HYPRLODR_CALL_HOOK_ALREADY: return "already_installed";
        case HYPRLODR_CALL_HOOK_BYTES_MISMATCH: return "callsite_mismatch";
        case HYPRLODR_CALL_HOOK_TARGET_MISMATCH: return "target_mismatch";
        case HYPRLODR_CALL_HOOK_RELAY_FAILED: return "relay_allocation_failed";
        case HYPRLODR_CALL_HOOK_PROTECT_FAILED: return "protection_failed";
        case HYPRLODR_CALL_HOOK_FLUSH_FAILED: return "instruction_cache_flush_failed";
        case HYPRLODR_CALL_HOOK_NOT_QUIESCENT: return "install_not_quiescent";
        default: return "install_failed";
    }
}

/* Called only for an explicitly opted-in process-start DLL attachment. The
 * non-NULL reserved argument distinguishes this path from LoadLibrary. */
static void install_early_dormant_route_hook(LPVOID reserved) {
    static const unsigned char expected_call[5] = {0xe8u, 0xa3u, 0x09u, 0x00u, 0x00u};
    if (!g_early_hook_requested) return;
    if (!reserved) {
        snprintf(g_early_hook_status, sizeof g_early_hook_status,
                 "rejected_dynamic_load");
        return;
    }
    inspect_main_module();
    if (!g_module.parsed || g_module.timestamp != HYPRLODR_GAME_BUILD_TIMESTAMP ||
        g_module.size_of_image != HYPRLODR_GAME_SIZE_OF_IMAGE ||
        g_anchors.total != HYPRLODR_GAME_ANCHOR_COUNT ||
        g_anchors.matched != HYPRLODR_GAME_ANCHOR_COUNT ||
        HYPRLODR_GAME_CALLSITE_RVA + sizeof expected_call > g_module.size_of_image) {
        snprintf(g_early_hook_status, sizeof g_early_hook_status,
                 "rejected_unsupported_executable");
        return;
    }
    if (!startup_import_layout_supported()) {
        snprintf(g_early_hook_status, sizeof g_early_hook_status,
                 "rejected_startup_import_or_tls_layout");
        return;
    }
    unsigned char *site = (unsigned char *)g_module.base + HYPRLODR_GAME_CALLSITE_RVA;
    if (!bytes_equal(site, expected_call, sizeof expected_call)) {
        snprintf(g_early_hook_status, sizeof g_early_hook_status,
                 "rejected_callsite_mismatch");
        return;
    }
    void *target = (void *)(g_module.base + HYPRLODR_GAME_LOADER_RVA);
    hyprlodr_record_route_hook_set_original_target(target);
    g_route_call_hook.original_target = target;
    hyprlodr_record_route_hook_bind(&g_route_binding, &g_route_call_hook, NULL);
    hyprlodr_record_route_hook_set_activation_gate(
        &g_route_binding, hyprlodr_file_hook_archive_gate_word());
    hyprlodr_call_hook_status status = hyprlodr_call_hook_install(
        &g_route_call_hook, site, expected_call, target,
        (void *)hyprlodr_record_route_hook_call,
        HYPRLODR_CALL_HOOK_INSTALL_QUIESCENT, NULL);
    snprintf(g_early_hook_status, sizeof g_early_hook_status, "%s",
             call_hook_status_text(status));
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

    fprintf(file, "\",\"proxy_build_revision\":\"%s\",\"proxy_build_clean\":%s,"
                  "\"proxy_capability_bits\":%u,\"proxy_capabilities\":\"%s\"",
            HYPRLODR_BUILD_REVISION, HYPRLODR_BUILD_CLEAN ? "true" : "false",
            (unsigned)HYPRLODR_PROXY_CAPABILITIES, HYPRLODR_PROXY_CAPABILITY_NAMES);

    fprintf(file, ",\"run_id\":\"%016llx\",\"diagnostics_requested\":%s,"
                  "\"diagnostic_writer_start_result\":%d,\"diagnostic_writer_status\":%d,"
                  "\"diagnostic_events_dropped\":%u",
            (unsigned long long)hyprlodr_startup_diag_run_id(),
            g_startup_diag_requested ? "true" : "false",
            g_diag_writer_start_result, hyprlodr_startup_diag_writer_status(),
            hyprlodr_startup_diag_dropped());

    fprintf(file, ",\"delay_ms\":%lu,\"module_parsed\":", (unsigned long)g_delay_ms);
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
    fputs(",\"experimental_record_route_requested\":", file);
    fputs(g_record_route_requested ? "true" : "false", file);
    fputs(",\"experimental_record_route_status\":\"", file);
    json_escape(file, g_record_route_status);
    fputs("\",\"experimental_early_hook_requested\":", file);
    fputs(g_early_hook_requested ? "true" : "false", file);
    fputs(",\"experimental_early_hook_status\":\"", file);
    json_escape(file, g_early_hook_status);
    fprintf(file, "\",\"early_hook_install_attempted\":%s,\"early_hook_installed\":%s",
            g_early_hook_requested ? "true" : "false",
            g_route_call_hook.installed ? "true" : "false");
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
    fprintf(file, "\",\"file_redirects\":%lu,\"file_opens_logged\":%d,\"gl_capture\":\"",
            (unsigned long)hyprlodr_loader_redirect_count(g_loader),
            hyprlodr_file_hook_logged());
    json_escape(file, hyprlodr_gl_hook_status());
    fprintf(file, "\",\"gl_capture_detail\":\"");
    json_escape(file, hyprlodr_gl_hook_detail());
    fprintf(file, "\"}\n");
    fclose(file);
}

#include "asset_probe.h"

static DWORD WINAPI bootstrap(LPVOID unused) {
    (void)unused;

    char startup_log[1024];
    if (g_startup_diag_requested) {
        snprintf(startup_log, sizeof startup_log,
                 "%s\\hyprlodr-startup-%016llx.jsonl", g_game_dir,
                 (unsigned long long)hyprlodr_startup_diag_run_id());
    } else {
        snprintf(startup_log, sizeof startup_log, "%s\\hyprlodr-startup.jsonl", g_game_dir);
    }
    g_diag_writer_start_result = hyprlodr_startup_diag_start(startup_log);
    hyprlodr_startup_diag_drain();

    /* Evidence and build identity first. A game that exits during start-up can
     * still prove which executable ran and whether it matches the analysis; the
     * mod start-up delay below is about the game being ready, not about us. */
    g_delay_ms = bootstrap_delay_ms();
    g_record_route_requested = bootstrap_record_route_requested();

    char mods_dir[1024];
    char disabled_file[1024];
    char log_path[1024];
    char state_root[1024];
    char files_log[1024];
    char gl_log[1024];
    snprintf(mods_dir, sizeof mods_dir, "%s\\mods", g_game_dir);
    snprintf(disabled_file, sizeof disabled_file, "%s\\mods\\disabled.txt", g_game_dir);
    snprintf(log_path, sizeof log_path, "%s\\hyprlodr-events.jsonl", g_game_dir);
    snprintf(state_root, sizeof state_root, "%s\\hyprlodr-state", g_game_dir);
    if (g_startup_diag_requested) {
        snprintf(files_log, sizeof files_log, "%s\\hyprlodr-files-%016llx.jsonl",
                 g_game_dir, (unsigned long long)hyprlodr_startup_diag_run_id());
    } else {
        snprintf(files_log, sizeof files_log, "%s\\hyprlodr-files.jsonl", g_game_dir);
    }
    snprintf(gl_log, sizeof gl_log, "%s\\hyprlodr-gl.jsonl", g_game_dir);

    g_loader = hyprlodr_loader_create();
    if (!g_loader) {
        write_attach_record("no_memory", HYPRLODR_ERR_LIMIT);
        hyprlodr_file_hook_mark_redirects_ready();
        hyprlodr_startup_diag_drain();
        return 0;
    }
    if (hyprlodr_loader_set_log(g_loader, log_path) != HYPRLODR_OK) {
        write_attach_record("log_open_failed", HYPRLODR_ERR_IO);
        hyprlodr_loader_destroy(g_loader);
        g_loader = NULL;
        hyprlodr_file_hook_mark_redirects_ready();
        hyprlodr_startup_diag_drain();
        return 0;
    }
    hyprlodr_loader_add_dir(g_loader, mods_dir);
    hyprlodr_loader_disable_file(g_loader, disabled_file);
    /* Per-mod writable directories live beside the loader's own files. */
    hyprlodr_loader_set_state_root(g_loader, state_root);

    /* ABI 5: offer the running image to mods, but only when the fingerprint
     * proves this executable is the build the verifier describes. A mod can then
     * inspect real code and data; a different build yields build_matches=0 and
     * every read is refused. */
    inspect_main_module();
    int build_matches = g_module.parsed &&
                        g_module.timestamp == HYPRLODR_GAME_BUILD_TIMESTAMP &&
                        g_anchors.total > 0 && g_anchors.matched == g_anchors.total;
    preflight_record_route(build_matches);
    hyprlodr_startup_diag_emit(HYPRLODR_DIAG_ROUTE_PREFLIGHT, 0u, 0u, 0u,
        (uint32_t)hyprlodr_file_hook_archive_gate_state(),
        (g_record_route_requested ? 1u : 0u) |
            (g_route_binding.route ? 2u : 0u),
        0u, 0u, g_record_route_status);
    hyprlodr_startup_diag_drain();
    hyprlodr_image image;
    image.base = g_module.base;
    image.size = (size_t)g_module.size_of_image;
    /* ABI 6: the proxy performs patches so it can lift page protection, and the
     * loader restores them when the process shuts down. */
    image.write_context = NULL;
    image.write = image_write_bytes;
    image.build_matches = g_module.parsed &&
                          g_module.timestamp == HYPRLODR_GAME_BUILD_TIMESTAMP &&
                          g_anchors.total > 0 && g_anchors.matched == g_anchors.total;
    hyprlodr_loader_set_image(g_loader, &image);

    /* ABI 7: publish the named, hash-verified locations the RE pipeline recorded
     * for this build, so mods ask for a name instead of carrying an RVA. Each
     * lookup re-hashes the live bytes. */
    hyprlodr_loader_set_symbols(g_loader, hyprlodr_game_anchors, HYPRLODR_GAME_ANCHOR_COUNT);

    /* Intercept the game's own CreateFileA calls before the delay, so opens the
     * game performs while mods are still starting are observed too. Nothing is
     * patched unless the import table really contains that entry. */
    hyprlodr_file_hook_install(g_loader, files_log);
    /* Research capture of the GL entry points GLFW resolves at run time. It only
     * records; nothing in the renderer is replaced. */
    hyprlodr_gl_hook_set_game_dir(g_game_dir);
    (void)hyprlodr_gl_hook_install(gl_log);
    /* Research probe: find the live asset records so the pack can be read. It
     * only reads memory and writes its own log. */
    {
        char assets_log[1024];
        snprintf(assets_log, sizeof assets_log, "%s\\hyprlodr-assets.jsonl", g_game_dir);
        (void)hyprlodr_asset_probe_start(g_game_dir, assets_log);
    }

    write_attach_record("attached", HYPRLODR_OK);

    if (g_delay_ms) Sleep(g_delay_ms);

    /* Rewriting the record with this stage proves the process outlived the
     * delay; a record still reading "attached" means it did not. */
    write_attach_record("initializing", HYPRLODR_OK);

    int status = hyprlodr_loader_resolve(g_loader);
    if (status == HYPRLODR_OK) status = hyprlodr_loader_init(g_loader);

    char redirect_count_text[64];
    snprintf(redirect_count_text, sizeof redirect_count_text,
             "loader status=%d redirects=%lu", status,
             (unsigned long)hyprlodr_loader_redirect_count(g_loader));
    hyprlodr_startup_diag_emit(HYPRLODR_DIAG_REDIRECT_PUBLICATION, 0u, 0u, 0u,
        (uint32_t)hyprlodr_file_hook_archive_gate_state(),
        status == HYPRLODR_OK ? 1u : 0u, 0u, 0u, redirect_count_text);
    hyprlodr_startup_diag_drain();

    /* The asset-open shim still holds the first archive open. Activate only
     * after manifest validation and publication of the matching archive
     * redirect, before waking any request that could perform a cold load. */
    if (g_route_call_hook.installed &&
        strcmp(g_record_route_status, "manifest_validated_waiting_for_redirect") == 0) {
        if (status != HYPRLODR_OK) {
            hyprlodr_call_hook_set_enabled(&g_route_call_hook, 0);
            snprintf(g_record_route_status, sizeof g_record_route_status,
                     "route_disabled_bootstrap_failed");
        } else {
            char redirected_archive[1024];
            if (!hyprlodr_loader_redirect_path(g_loader, "asset/asset",
                                                redirected_archive,
                                                sizeof redirected_archive)) {
                snprintf(g_record_route_status, sizeof g_record_route_status,
                         "route_blocked_archive_redirect_missing");
            } else {
                int same_path = 1;
                const char *left = g_route_archive_path;
                const char *right = redirected_archive;
                while (*left && *right) {
                    char a = *left == '/' ? '\\' : *left;
                    char b = *right == '/' ? '\\' : *right;
                    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                    if (a != b) { same_path = 0; break; }
                    ++left; ++right;
                }
                if (*left || *right) same_path = 0;
                if (!same_path) {
                    snprintf(g_record_route_status, sizeof g_record_route_status,
                             "route_blocked_archive_redirect_mismatch");
                } else {
                    if (!hyprlodr_file_hook_begin_route_activation()) {
                        snprintf(g_record_route_status, sizeof g_record_route_status,
                                 "route_blocked_archive_open_fallback");
                    } else {
                        hyprlodr_call_hook_set_enabled(&g_route_call_hook, 1);
                        if (hyprlodr_file_hook_complete_route_activation()) {
                            snprintf(g_record_route_status, sizeof g_record_route_status,
                                     "route_enabled");
                        } else {
                            hyprlodr_call_hook_set_enabled(&g_route_call_hook, 0);
                            snprintf(g_record_route_status, sizeof g_record_route_status,
                                     "route_blocked_archive_open_fallback");
                        }
                    }
                }
            }
        }
    }

    /* Wake an early asset open even when mod resolution or initialization
     * failed. The file shim then proceeds with the original path instead of
     * holding the game thread indefinitely. */
    hyprlodr_file_hook_mark_redirects_ready();
    hyprlodr_startup_diag_emit(HYPRLODR_DIAG_ROUTE_ACTIVATION, 0u, 0u, 0u,
        (uint32_t)hyprlodr_file_hook_archive_gate_state(),
        strcmp(g_record_route_status, "route_enabled") == 0 ? 1u : 0u,
        0u, 0u, g_record_route_status);
    hyprlodr_startup_diag_drain();

    /* The loader's own cap is internal; asking for more ids than it can hold is
     * harmless and keeps this call free of a private constant. */

    write_attach_record(status == HYPRLODR_OK ? "mods_loaded" : "mods_failed", status);
    hyprlodr_startup_diag_drain();
    return 0;
}

BOOL WINAPI dll_main(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        char module_path[1024];
        module_path[0] = '\0';
        GetModuleFileNameA((HMODULE)instance, module_path, (DWORD)sizeof module_path);
        snprintf(g_game_dir, sizeof g_game_dir, "%s", module_path);
        trim_filename(g_game_dir);
        DisableThreadLibraryCalls((HMODULE)instance);
        uint64_t run_id = ((uint64_t)GetCurrentProcessId() << 32) | GetTickCount();
        hyprlodr_startup_diag_set_run_id(run_id);
        g_startup_diag_requested = startup_diag_requested();
        hyprlodr_startup_diag_enable(g_startup_diag_requested);
        if (g_startup_diag_requested) {
            snprintf(g_evidence_path, sizeof g_evidence_path,
                     "%s\\hyprlodr-attach-%016llx.json", g_game_dir,
                     (unsigned long long)run_id);
        } else {
            snprintf(g_evidence_path, sizeof g_evidence_path,
                     "%s\\hyprlodr-attach.json", g_game_dir);
        }
        g_early_hook_requested = early_record_route_requested();
        install_early_dormant_route_hook(reserved);
        hyprlodr_startup_diag_emit_once(HYPRLODR_DIAG_EARLY_HOOK, 0u, 0u, 0u,
            (uint32_t)hyprlodr_file_hook_archive_gate_state(),
            (g_early_hook_requested ? 1u : 0u) |
                (g_route_call_hook.installed ? 2u : 0u),
            0u, 0u, g_early_hook_status);
        /* Chain, do not replace: Steam's crash handler stays in charge. */
        g_previous_filter = SetUnhandledExceptionFilter(hyprlodr_crash_filter);
        /* Patch the executable's own import slots *now*: hyperdemon.exe opens its
         * asset before the worker thread below gets scheduled, so a hook installed
         * later silently misses the game's first (and often only) file I/O. The
         * loader does not exist yet, which is fine: no redirect is published. */
        char early_log[1024];
        snprintf(early_log, sizeof early_log, "%s\\hyprlodr-files.jsonl", g_game_dir);
        (void)hyprlodr_file_hook_install_early(NULL, early_log);
        /* GLFW resolves wglGetProcAddress through this slot during context
         * creation, which happens long before the bootstrap thread runs. */
        char early_gl_log[1024];
        snprintf(early_gl_log, sizeof early_gl_log, "%s\\hyprlodr-gl.jsonl", g_game_dir);
        (void)hyprlodr_gl_hook_install_early(early_gl_log);
        /* Never run mod code or file I/O for mods under the loader lock. The
         * file shim can wait on the worker if the game reaches asset/asset
         * before the redirect registry is published. */
        if (!CreateThread(NULL, 0, bootstrap, NULL, 0, NULL)) {
            /* Do not make the game's first archive open wait for the full gate
             * timeout when bootstrap could not start at all. No redirect can
             * be published, so the file shim safely falls back to the source. */
            hyprlodr_file_hook_mark_redirects_ready();
        }
    } else if (reason == DLL_PROCESS_DETACH && reserved == NULL) {
        hyprlodr_startup_diag_request_stop();
        if (g_loader) {
            hyprlodr_loader_shutdown(g_loader);
            hyprlodr_loader_destroy(g_loader);
            g_loader = NULL;
        }
    }
    return TRUE;
}
