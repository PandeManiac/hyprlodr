/* Wine test mod for ABI 4 file redirection.
 *
 * It writes its replacement file into the state directory the loader reserves,
 * then asks the host to open that file instead of "redirect-probe.dat". The
 * stand-in host process (proxy_client.exe) opens the original name afterwards
 * and must receive these bytes through the patched CreateFileA import slot.
 */
#include "hyprlodr/plugin.h"
#include "hypr_win32.h"

#include <stdio.h>
#include <string.h>

#define MOD_ID "r.redirect"
#define MOD_VERSION "1.0.0"
#define PROBE_NAME "redirect-probe.dat"
#define ASSET_NAME "asset/asset"
#define PROBE_CONTENT "hyprlodr redirect content"

static int mod_initialize(const hyprlodr_host_api *host) {
#ifdef HYPRLODR_TEST_INIT_SLEEP_MS
    Sleep(HYPRLODR_TEST_INIT_SLEEP_MS);
#endif
    if (!HYPRLODR_HOST_API_HAS(host, file_redirect) || !host->file_redirect) {
        host->log(host->context, "error", MOD_ID, "host has no file_redirect");
        return -1;
    }
    const char *directory = HYPRLODR_HOST_API_HAS(host, state_dir) && host->state_dir
                                ? host->state_dir(host->context)
                                : NULL;
    if (!directory || !*directory) {
        host->log(host->context, "error", MOD_ID, "state_dir unavailable");
        return -1;
    }
    char replacement[1024];
    snprintf(replacement, sizeof replacement, "%s\\replacement.txt", directory);
    FILE *file = fopen(replacement, "w");
    if (!file) {
        host->log(host->context, "error", MOD_ID, "cannot write replacement file");
        return -1;
    }
    fputs(PROBE_CONTENT, file);
    fclose(file);

#ifndef HYPRLODR_TEST_NO_REDIRECT
    if (host->file_redirect(host->context, PROBE_NAME, replacement) != 0) {
        host->log(host->context, "error", MOD_ID, "file_redirect refused");
        return -1;
    }
    if (host->file_redirect(host->context, ASSET_NAME, replacement) != 0) {
        host->log(host->context, "error", MOD_ID, "asset archive redirect refused");
        return -1;
    }
#ifdef HYPRLODR_TEST_FAIL_AFTER_REDIRECT
    host->log(host->context, "error", MOD_ID, "intentional initialization failure after redirects");
    return -1;
#endif
#endif
    char message[1200];
    snprintf(message, sizeof message, "%s -> %s", PROBE_NAME, replacement);
    host->log(host->context, "info", MOD_ID, message);
    return 0;
}

static void mod_shutdown(const hyprlodr_host_api *host) {
    host->log(host->context, "info", MOD_ID, "shutdown complete");
}

static const hyprlodr_plugin_descriptor descriptor = {
    (uint32_t)sizeof descriptor,
    HYPRLODR_ABI_VERSION,
    MOD_ID,
    MOD_VERSION,
    NULL,
    0,
    mod_initialize,
    mod_shutdown,
    NULL,
    0,
    "Redirect probe",
    "Registers a file redirect the Wine stand-in host verifies.",
    "HYPRLODR tests",
    HYPRLODR_PLUGIN_FLAG_DIAGNOSTIC,
};

HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version) {
    return host_abi_version == HYPRLODR_ABI_VERSION ? &descriptor : NULL;
}
