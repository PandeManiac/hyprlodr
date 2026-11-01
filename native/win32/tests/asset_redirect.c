/* In-game asset redirection probe (ABI 4, now on a path the game really uses).
 *
 * Registers a suffix redirect for the game's asset container to a file inside
 * this mod's own state directory. The file is a hardlink to the original, so the
 * bytes are identical: the game must still start and run, which makes this a
 * safe proof that substitution reaches the game's loader rather than a test that
 * depends on changed content.
 */
#include "hyprlodr/plugin.h"

#include <stdio.h>
#include <string.h>

#define MOD_ID "r.asset"
#define MOD_VERSION "1.0.0"
#define ASSET_SUFFIX "asset/asset"

static int mod_initialize(const hyprlodr_host_api *host) {
    if (!HYPRLODR_HOST_API_HAS(host, file_redirect) || !host->file_redirect) {
        host->log(host->context, "error", MOD_ID, "file_redirect unavailable");
        return -1;
    }
    if (!HYPRLODR_HOST_API_HAS(host, state_dir) || !host->state_dir) {
        host->log(host->context, "error", MOD_ID, "no state directory");
        return -1;
    }
    const char *directory = host->state_dir(host->context);
    if (!directory || !*directory) {
        host->log(host->context, "error", MOD_ID, "no state directory");
        return -1;
    }

    char replacement[1024];
    int written = snprintf(replacement, sizeof replacement, "%s\\asset.bin", directory);
    if (written < 0 || (size_t)written >= sizeof replacement) {
        host->log(host->context, "error", MOD_ID, "replacement path too long");
        return -1;
    }
    if (host->file_redirect(host->context, ASSET_SUFFIX, replacement) != 0) {
        host->log(host->context, "error", MOD_ID, "asset redirect refused");
        return -1;
    }
    host->log(host->context, "info", MOD_ID, "asset redirect registered");
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
    "Asset redirect probe",
    "Redirects the game's asset container to a byte-identical copy.",
    "HYPRLODR tests",
    HYPRLODR_PLUGIN_FLAG_DIAGNOSTIC,
};

HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version) {
    return host_abi_version == HYPRLODR_ABI_VERSION ? &descriptor : NULL;
}
