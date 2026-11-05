#define HYPRLODR_MINIMAL_WIN32 1
#include "hypr_win32.h"
#include "file_hook.h"
#include "loader.h"

#include <stdio.h>

static HANDLE g_release_event;
static volatile LONG g_status;
static volatile LONG g_route_enabled;
static hyprlodr_loader *g_loader;

static int env_is_one(const char *name) {
    char value[8];
    DWORD length = GetEnvironmentVariableA(name, value, (DWORD)sizeof value);
    return length == 1u && value[0] == '1' && value[1] == '\0';
}

static void finish_bootstrap(int status) {
    __atomic_store_n(&g_status, (LONG)status, __ATOMIC_RELEASE);
    hyprlodr_file_hook_mark_redirects_ready();
}

static DWORD WINAPI startup_file_bootstrap(LPVOID unused) {
    (void)unused;
    if (WaitForSingleObject(g_release_event, 30000u) != 0u) {
        finish_bootstrap(3);
        return 0;
    }
    if (env_is_one("HYPRLODR_FILE_FIXTURE_FAIL")) {
        finish_bootstrap(2);
        return 0;
    }
    g_loader = hyprlodr_loader_create();
    if (!g_loader || hyprlodr_loader_set_log(g_loader, "startup-file-events.jsonl") != HYPRLODR_OK ||
        hyprlodr_loader_add_dir(g_loader, "mods") != HYPRLODR_OK ||
        hyprlodr_loader_set_state_root(g_loader, "startup-file-state") != HYPRLODR_OK ||
        hyprlodr_loader_resolve(g_loader) != HYPRLODR_OK ||
        hyprlodr_loader_init(g_loader) != HYPRLODR_OK) {
        finish_bootstrap(4);
        return 0;
    }
    (void)hyprlodr_file_hook_install(g_loader, "startup-file-order.jsonl");
    char redirected[HYPRLODR_REDIRECT_PATH_MAX];
    if (hyprlodr_loader_redirect_path(g_loader, "asset/asset", redirected,
                                      sizeof redirected)) {
        if (!hyprlodr_file_hook_begin_route_activation()) {
            finish_bootstrap(6);
            return 0;
        }
        __atomic_store_n(&g_route_enabled, 1l, __ATOMIC_RELEASE);
        if (!hyprlodr_file_hook_complete_route_activation()) {
            __atomic_store_n(&g_route_enabled, 0l, __ATOMIC_RELEASE);
            finish_bootstrap(6);
            return 0;
        }
    }
    finish_bootstrap(1);
    return 0;
}

void startup_file_fixture_release(void) {
    if (g_release_event) SetEvent(g_release_event);
}

LONG startup_file_fixture_route_enabled(void) {
    return __atomic_load_n(&g_route_enabled, __ATOMIC_ACQUIRE);
}

LONG startup_file_fixture_status(void) {
    return __atomic_load_n(&g_status, __ATOMIC_ACQUIRE);
}

unsigned startup_file_fixture_waiters(void) {
    return hyprlodr_file_hook_archive_waiters();
}

BOOL WINAPI startup_file_fixture_dllmain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    if (reason == DLL_PROCESS_ATTACH) {
        g_release_event = CreateEventA(NULL, TRUE, FALSE, NULL);
        (void)hyprlodr_file_hook_install_early(NULL, "startup-file-order.jsonl");
        if (!g_release_event || env_is_one("HYPRLODR_FILE_FIXTURE_WORKER_FAIL")) {
            finish_bootstrap(5);
            return TRUE;
        }
        if (!CreateThread(NULL, 0, startup_file_bootstrap, NULL, 0, NULL))
            finish_bootstrap(5);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_release_event) {
            SetEvent(g_release_event);
            CloseHandle(g_release_event);
            g_release_event = NULL;
        }
        if (g_loader) {
            hyprlodr_loader_shutdown(g_loader);
            hyprlodr_loader_destroy(g_loader);
            g_loader = NULL;
        }
    }
    (void)reserved;
    return TRUE;
}
