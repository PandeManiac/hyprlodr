/* ABI 6 in-game probe: patch one byte of the fingerprint-verified string, read
 * it back, and restore it in the same call.
 *
 * The point is to prove that patching a real mapped image works inside the game
 * process — page protection, write, second read — while leaving no trace: the
 * byte is restored before initialize() returns, and the loader would restore it
 * on shutdown anyway. It logs only hashes and counts, never game bytes.
 */
#include "hyprlodr/plugin.h"

#include <stdio.h>
#include <string.h>

#define MOD_ID "i.writer"
#define MOD_VERSION "1.0.0"

#define PROBE_RVA 0x0036f640ul
#define PROBE_LENGTH 12ul
#define PROBE_FNV 0x935e7de761104fdcull

static uint64_t fnv1a64(const unsigned char *data, size_t length) {
    uint64_t value = 0xcbf29ce484222325ull;
    for (size_t index = 0; index < length; ++index) {
        value ^= (uint64_t)data[index];
        value *= 0x100000001b3ull;
    }
    return value;
}

static int mod_initialize(const hyprlodr_host_api *host) {
    char message[128];

    if (!HYPRLODR_HOST_API_HAS(host, image_write) || !host->image_write ||
        !HYPRLODR_HOST_API_HAS(host, image_restore) || !host->image_restore) {
        host->log(host->context, "error", MOD_ID, "image_write unavailable");
        return -1;
    }
    if (!host->build_matches || !host->build_matches(host->context)) {
        host->log(host->context, "warn", MOD_ID, "image is not the verified build; not patching");
        return 0;
    }

    unsigned char original[PROBE_LENGTH];
    memset(original, 0, sizeof original);
    if (host->image_read(host->context, (uint64_t)PROBE_RVA, original, sizeof original) != 0) {
        host->log(host->context, "warn", MOD_ID, "probe read refused");
        return 0;
    }
    if (fnv1a64(original, sizeof original) != PROBE_FNV) {
        host->log(host->context, "error", MOD_ID, "probe fingerprint mismatch");
        return 0;
    }

    /* Flip the first byte of a string the game has already loaded, then put it
     * back within this call. A patch nobody reads is still a patch: the second
     * read is what proves the write reached the image. */
    unsigned char patch = (unsigned char)(original[0] ^ 0xff);
    if (host->image_write(host->context, (uint64_t)PROBE_RVA, &patch, 1) != 0) {
        host->log(host->context, "warn", MOD_ID, "probe write refused");
        return 0;
    }
    unsigned char observed = 0;
    if (host->image_read(host->context, (uint64_t)PROBE_RVA, &observed, 1) != 0 ||
        observed != patch) {
        host->log(host->context, "error", MOD_ID, "patch not visible through a second read");
        (void)host->image_restore(host->context);
        return 0;
    }
    host->log(host->context, "info", MOD_ID, "patch visible through a second read");

    size_t restored = host->image_restore(host->context);
    snprintf(message, sizeof message, "restored %lu byte(s)", (unsigned long)restored);
    host->log(host->context, "info", MOD_ID, message);

    unsigned char check[PROBE_LENGTH];
    memset(check, 0, sizeof check);
    if (host->image_read(host->context, (uint64_t)PROBE_RVA, check, sizeof check) == 0 &&
        fnv1a64(check, sizeof check) == PROBE_FNV)
        host->log(host->context, "info", MOD_ID, "original bytes confirmed after restore");
    else
        host->log(host->context, "error", MOD_ID, "restore did not bring the original back");
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
    "Image writer",
    "Patches and restores one verified byte of the running game.",
    "HYPRLODR tests",
    HYPRLODR_PLUGIN_FLAG_DIAGNOSTIC,
};

HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version) {
    return host_abi_version == HYPRLODR_ABI_VERSION ? &descriptor : NULL;
}
