#define HYPRLODR_MINIMAL_WIN32 1
#include "hypr_win32.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static volatile LONG g_calls;
static char g_last_path[1024];

__declspec(dllexport) FILE *fixture_fiopen(const char *path, int mode, int protection) {
    (void)mode;
    (void)protection;
    if (!path) return NULL;
    size_t length = strlen(path);
    if (length >= sizeof g_last_path) length = sizeof g_last_path - 1u;
    memcpy(g_last_path, path, length);
    g_last_path[length] = '\0';
    __atomic_add_fetch(&g_calls, 1l, __ATOMIC_RELAXED);
    return (FILE *)(uintptr_t)0x1234u;
}

__declspec(dllexport) LONG fixture_fiopen_call_count(void) {
    return __atomic_load_n(&g_calls, __ATOMIC_ACQUIRE);
}

__declspec(dllexport) const char *fixture_fiopen_last_path(void) {
    return g_last_path;
}
