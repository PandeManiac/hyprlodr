#define HYPRLODR_MINIMAL_WIN32 1
#include "hypr_win32.h"
#include "callsite_hook.h"
#include "record_route_hook.h"

#include <string.h>

typedef uintptr_t (*fixture_fn)(void *, void *, const void *, uint32_t);

static hyprlodr_call_hook g_hook = HYPRLODR_CALL_HOOK_INITIALIZER;
static hyprlodr_game_route_binding g_binding;
static hyprlodr_record_route g_route = {
    "hand2", 295758784u, 21972u, 1u, 306794369u, 22056u, 306816425u
};
static volatile LONG g_status;

enum {
    FIXTURE_NOT_REQUESTED = 0,
    FIXTURE_DORMANT_INSTALLED = 1,
    FIXTURE_REJECTED_DYNAMIC = 2,
    FIXTURE_REJECTED_SIGNATURE = 3,
    FIXTURE_INSTALL_FAILED = 4
};

int startup_fixture_hook_status(void) {
    return __atomic_load_n(&g_status, __ATOMIC_ACQUIRE);
}

void startup_fixture_enable(void) {
    hyprlodr_record_route_hook_publish_route(&g_binding, &g_route);
    hyprlodr_call_hook_set_enabled(&g_hook, 1);
}

void startup_fixture_disable(void) {
    hyprlodr_call_hook_set_enabled(&g_hook, 0);
}

void *startup_fixture_hook_site(void) {
    return g_hook.site;
}

static void install_dormant(LPVOID reserved) {
    static const char opt_in[] = "HYPRLODR_FIXTURE_EARLY";
    char value[8];
    if (GetEnvironmentVariableA(opt_in, value, (DWORD)sizeof value) != 1u ||
        value[0] != '1' || value[1] != '\0') return;
    if (!reserved) {
        __atomic_store_n(&g_status, FIXTURE_REJECTED_DYNAMIC, __ATOMIC_RELEASE);
        return;
    }

    HMODULE executable = GetModuleHandleA(NULL);
    if (!executable) {
        __atomic_store_n(&g_status, FIXTURE_INSTALL_FAILED, __ATOMIC_RELEASE);
        return;
    }
    unsigned char *entry = (unsigned char *)GetProcAddress(executable,
                                                            "startup_fixture_callsite");
    void *original_target = (void *)GetProcAddress(executable,
                                                    "startup_fixture_original");
    if (!entry || !original_target) {
        __atomic_store_n(&g_status, FIXTURE_INSTALL_FAILED, __ATOMIC_RELEASE);
        return;
    }
    unsigned char *site = entry + 4u; /* sub rsp, 0x28; CALL rel32 */
    unsigned char expected[5];
    memcpy(expected, site, sizeof expected);
    if (expected[0] != 0xe8u) {
        __atomic_store_n(&g_status, FIXTURE_REJECTED_SIGNATURE, __ATOMIC_RELEASE);
        return;
    }
    if (GetEnvironmentVariableA("HYPRLODR_FIXTURE_BAD_BYTES", value,
                                (DWORD)sizeof value) == 1u && value[0] == '1')
        expected[0] = 0xe9u;

    hyprlodr_record_route_hook_set_original_target(original_target);
    g_hook.original_target = original_target;
    hyprlodr_record_route_hook_bind(&g_binding, &g_hook, NULL);
    hyprlodr_call_hook_status status = hyprlodr_call_hook_install(
        &g_hook, site, expected, original_target,
        (void *)hyprlodr_record_route_hook_call,
        HYPRLODR_CALL_HOOK_INSTALL_QUIESCENT, NULL);
    __atomic_store_n(&g_status,
                     status == HYPRLODR_CALL_HOOK_OK ||
                             status == HYPRLODR_CALL_HOOK_ALREADY
                         ? FIXTURE_DORMANT_INSTALLED : FIXTURE_INSTALL_FAILED,
                     __ATOMIC_RELEASE);
}

BOOL WINAPI startup_fixture_dllmain(HINSTANCE instance, DWORD reason,
                                     LPVOID reserved) {
    (void)instance;
    if (reason == DLL_PROCESS_ATTACH) install_dormant(reserved);
    return TRUE;
}
