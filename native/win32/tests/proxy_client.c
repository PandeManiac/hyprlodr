/* Stand-in for the game process: loads OpenAL32.dll by name and resolves
 * forwarded exports, proving the proxy replaces the original library and that
 * its bootstrap attaches without requiring HYPER DEMON itself. */
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
    /* Give the loader bootstrap thread time to run before the process exits. */
    Sleep(6000);
    return (al_get_error && alc_open_device && al_gen_sources) ? 0 : 2;
}
