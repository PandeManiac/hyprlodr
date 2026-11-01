/* Stand-in for the game process: loads OpenAL32.dll by name and resolves
 * forwarded exports, proving the proxy replaces the original library and that
 * its bootstrap attaches without requiring HYPER DEMON itself.
 *
 * It then opens a path a test mod redirected. That call goes through this
 * executable's own KERNEL32!CreateFileA import slot, which the proxy patches, so
 * the bytes that come back prove the in-game file hook end to end. */
#include "hypr_win32.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    HMODULE proxy = LoadLibraryA("OpenAL32.dll");
    if (!proxy) {
        printf("proxy_client: LoadLibraryA(OpenAL32.dll) failed\n");
        return 3;
    }
    FARPROC al_get_error = GetProcAddress(proxy, "alGetError");
    FARPROC alc_open_device = GetProcAddress(proxy, "alcOpenDevice");
    FARPROC al_gen_sources = GetProcAddress(proxy, "alGenSources");
    printf("proxy_client: proxy=%d alGetError=%d alcOpenDevice=%d alGenSources=%d\n",
           proxy ? 1 : 0, al_get_error ? 1 : 0, alc_open_device ? 1 : 0,
           al_gen_sources ? 1 : 0);
    /* Give the loader bootstrap thread time to initialize mods (the built-in
     * delay is 2500 ms, and a config file may set it to 0). */
    Sleep(4000);

    HANDLE probe = CreateFileA("redirect-probe.dat", GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (probe == INVALID_HANDLE_VALUE) {
        printf("redirect-probe: open failed\n");
    } else {
        char content[128];
        DWORD read = 0;
        if (ReadFile(probe, content, (DWORD)sizeof content - 1u, &read, NULL)) {
            content[read] = '\0';
            printf("redirect-probe: %s\n", content);
        } else {
            printf("redirect-probe: read failed\n");
        }
        CloseHandle(probe);
    }

    Sleep(2500);
    return (al_get_error && alc_open_device && al_gen_sources) ? 0 : 2;
}
