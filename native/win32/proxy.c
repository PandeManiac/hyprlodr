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

#include <stdio.h>
#include <string.h>

#ifndef HYPRLODR_BOOTSTRAP_DELAY_MS
/* Let the game finish its own start-up before mods initialize. */
#  define HYPRLODR_BOOTSTRAP_DELAY_MS 2500
#endif

static hyprlodr_loader *g_loader;
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

static void write_attach_record(const char *stage, int status) {
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
    fputs("\"}\n", file);
    fclose(file);
}

static DWORD WINAPI bootstrap(LPVOID unused) {
    (void)unused;
    Sleep(HYPRLODR_BOOTSTRAP_DELAY_MS);

    char mods_dir[1024];
    char disabled_file[1024];
    char log_path[1024];
    snprintf(mods_dir, sizeof mods_dir, "%s\\mods", g_game_dir);
    snprintf(disabled_file, sizeof disabled_file, "%s\\mods\\disabled.txt", g_game_dir);
    snprintf(log_path, sizeof log_path, "%s\\hyprlodr-events.jsonl", g_game_dir);

    write_attach_record("attached", HYPRLODR_OK);

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
