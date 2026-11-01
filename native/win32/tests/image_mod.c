/* In-game fixture for ABI 5 image access.
 *
 * Reads the first bytes of the running image through the host API. Offset 0 is
 * the DOS header, so the result is "4d5a" for any PE image and needs no
 * game-specific offsets. In a host without a verified image (the Wine stand-in)
 * the read must be refused, which is what makes this a safety test as well as a
 * capability test.
 */
#include "hyprlodr/plugin.h"

#include <stdio.h>

#define MOD_ID "i.image"
#define MOD_VERSION "1.0.0"

static int mod_initialize(const hyprlodr_host_api *host) {
    if (!HYPRLODR_HOST_API_HAS(host, image_read) || !host->image_read) {
        host->log(host->context, "error", MOD_ID, "image_read unavailable");
        return -1;
    }
    unsigned char bytes[8];
    for (int index = 0; index < 8; ++index) bytes[index] = 0;

    if (!host->build_matches || !host->build_matches(host->context)) {
        host->log(host->context, "warn", MOD_ID, "image read refused");
        return 0;
    }
    if (host->image_read(host->context, 0, bytes, sizeof bytes) != 0) {
        host->log(host->context, "warn", MOD_ID, "image read refused");
        return 0;
    }
    char message[64];
    snprintf(message, sizeof message, "image header %02x%02x%02x%02x", bytes[0], bytes[1], bytes[2],
             bytes[3]);
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
    "Image probe",
    "Reads the running image header through ABI 5.",
    "HYPRLODR tests",
    HYPRLODR_PLUGIN_FLAG_DIAGNOSTIC,
};

HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version) {
    return host_abi_version == HYPRLODR_ABI_VERSION ? &descriptor : NULL;
}
