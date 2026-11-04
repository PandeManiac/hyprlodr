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
    printf("proxy_client: entered\n");
    fflush(NULL);
    HMODULE proxy = LoadLibraryA("OpenAL32.dll");
    if (!proxy) {
        printf("proxy_client: LoadLibraryA(OpenAL32.dll) failed\n");
        return 3;
    }
    printf("proxy_client: loaded\n");
    fflush(NULL);
    printf("proxy_client: proxy loaded\n");
    fflush(NULL);

    /* This first asset open deliberately races the worker. The proxy's narrow
     * gate must hold it until redirect publication and return the replacement
     * bytes before the default 2500 ms delay expires. Let the worker finish
     * hook setup and record its attach state, but trigger before mod loading. */
    Sleep(1000);
    printf("asset-open: requesting original path\n");
    fflush(NULL);
    HANDLE asset = CreateFileA("asset/asset", GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    int asset_ok = 0;
    if (asset == INVALID_HANDLE_VALUE) {
        printf("asset-open: failed\n");
    } else {
        char asset_content[128];
        DWORD asset_read = 0;
        if (!ReadFile(asset, asset_content, (DWORD)sizeof asset_content - 1u, &asset_read, NULL)) {
            printf("asset-open: read failed\n");
        } else {
            asset_content[asset_read] = '\0';
            printf("asset-open: %s\n", asset_content);
            asset_ok = 1;
        }
        CloseHandle(asset);
    }

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
    return asset_ok ? 0 : 2;
}
