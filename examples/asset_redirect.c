/* Diagnostic archive redirect used by the native hand capture workflow.
 * The target path is supplied in the mod's private state directory, so the DLL
 * can be rebuilt once and reused without embedding a user's local path. */
#include "hyprlodr/plugin.h"

#include <stdio.h>
#include <string.h>

#define MOD_ID "example.asset-redirect"
#define MOD_VERSION "1.0.0"

static int mod_initialize(const hyprlodr_host_api *host) {
    char path[1024];
    const char *directory;
    if (!HYPRLODR_HOST_API_HAS(host, file_redirect) || !host->file_redirect ||
        !HYPRLODR_HOST_API_HAS(host, state_dir) || !host->state_dir) return -1;
    directory = host->state_dir(host->context);
    if (!directory || !*directory) return -1;
    int length = snprintf(path, sizeof path, "%s/asset-redirect.txt", directory);
    if (length <= 0 || (size_t)length >= sizeof path) return -1;
    FILE *file = fopen(path, "r");
    if (!file) {
        host->log(host->context, "warn", MOD_ID, "asset-redirect.txt is missing");
        return -1;
    }
    char replacement[1024];
    if (!fgets(replacement, sizeof replacement, file)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    size_t size = strlen(replacement);
    while (size && (replacement[size - 1] == '\r' || replacement[size - 1] == '\n' ||
                    replacement[size - 1] == ' ' || replacement[size - 1] == '\t'))
        replacement[--size] = '\0';
    if (!size || replacement[0] != 'Z' || replacement[1] != ':') return -1;
    if (host->file_redirect(host->context, "asset/asset", replacement) != 0) return -1;
    host->log(host->context, "info", MOD_ID, "asset/asset redirect registered");
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
    "Asset redirect diagnostic",
    "Registers an asset archive path supplied in its private state directory.",
    "HYPRLODR",
    HYPRLODR_PLUGIN_FLAG_DIAGNOSTIC,
};

HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version) {
    return host_abi_version == HYPRLODR_ABI_VERSION ? &descriptor : NULL;
}
