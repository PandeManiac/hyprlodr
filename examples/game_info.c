/*
 * HYPRLODR worked example: example.game-info.
 *
 * The first mod that *reads the game*, rather than only running beside it. ABI 5
 * lets a mod read the running image, but only when the host has proved the
 * executable is the build the verifier describes. This example shows the whole
 * pattern a data-driven mod needs:
 *
 *   1. gate on build_matches() before touching any address;
 *   2. read a small range through image_read() and handle refusal as normal;
 *   3. hash what came back and compare it with a constant recorded during
 *      reverse engineering, so the mod fails loudly on a build it does not know.
 *
 * It reads the loader's own "resource path" anchor and recomputes its FNV-1a 64
 * hash. On the build this repository was analysed against, the value equals the
 * verifier's constant, which proves the bytes came from the game's own image and
 * not from anything the mod assumed. On any other build the host refuses the
 * read (build_matches() is 0) or the hash differs, and the mod logs that instead
 * of acting on data it cannot trust.
 *
 * The example deliberately logs a hash, never the bytes: evidence stays free of
 * game content. See docs/MOD_API.md.
 */
#include "hyprlodr/plugin.h"

#include <stdio.h>
#include <string.h>

#define MOD_ID "example.game-info"
#define MOD_VERSION "1.0.0"

/* The location is requested by *name* (ABI 7), so this example carries no RVA at
 * all: the host publishes the entries its pipeline recorded for the running
 * build and re-hashes the bytes on every lookup. The hash below is the mod's own
 * independent check that the bytes are the ones this example was written for. */
#define GAME_SYMBOL_RESOURCE_PATH "string:resource_path"
#define GAME_RESOURCE_PATH_LENGTH 12ul
#define GAME_RESOURCE_PATH_FNV 0x935e7de761104fdcull

/* FNV-1a 64, the same hash the host's verifier uses. Written out rather than
 * linked so a mod needs nothing but the public header. */
static uint64_t fnv1a64(const unsigned char *data, size_t length) {
    uint64_t value = 0xcbf29ce484222325ull;
    for (size_t index = 0; index < length; ++index) {
        value ^= (uint64_t)data[index];
        value *= 0x100000001b3ull;
    }
    return value;
}

static int mod_initialize(const hyprlodr_host_api *host) {
    char message[160];

    if (!HYPRLODR_HOST_API_HAS(host, image_read) || !host->image_read ||
        !HYPRLODR_HOST_API_HAS(host, build_matches) || !host->build_matches ||
        !HYPRLODR_HOST_API_HAS(host, image_size) || !host->image_size) {
        host->log(host->context, "warn", MOD_ID, "host has no image access; nothing to read");
        return 0;
    }

    /* The gate. Never read without it: on an unknown build every offset below is
     * a guess, and guessing at an address is how a mod crashes the game. */
    if (!host->build_matches(host->context)) {
        host->log(host->context, "warn", MOD_ID, "image is not the verified build; not reading");
        return 0;
    }

    size_t size = host->image_size(host->context);
    snprintf(message, sizeof message, "image %llu bytes verified",
             (unsigned long long)size);
    host->log(host->context, "info", MOD_ID, message);

    if (!HYPRLODR_HOST_API_HAS(host, symbol_lookup) || !host->symbol_lookup) {
        host->log(host->context, "warn", MOD_ID, "host publishes no symbol table");
        return 0;
    }
    uint64_t rva = 0;
    size_t length = 0;
    if (host->symbol_lookup(host->context, GAME_SYMBOL_RESOURCE_PATH, &rva, &length) != 0) {
        host->log(host->context, "warn", MOD_ID, "symbol lookup refused; not reading game data");
        return 0;
    }
    snprintf(message, sizeof message, "symbol %s at rva %lx (%lu bytes)",
             GAME_SYMBOL_RESOURCE_PATH, (unsigned long)rva, (unsigned long)length);
    host->log(host->context, "info", MOD_ID, message);

    if (length != GAME_RESOURCE_PATH_LENGTH) {
        host->log(host->context, "error", MOD_ID, "symbol length differs from this build");
        return 0;
    }
    unsigned char bytes[GAME_RESOURCE_PATH_LENGTH];
    memset(bytes, 0, sizeof bytes);
    if (host->image_read(host->context, rva, bytes, sizeof bytes) != 0) {
        host->log(host->context, "warn", MOD_ID, "resource path read refused");
        return 0;
    }

    uint64_t hash = fnv1a64(bytes, sizeof bytes);
    if (hash == GAME_RESOURCE_PATH_FNV) {
        snprintf(message, sizeof message, "resource path anchor verified at rva %lx",
                 (unsigned long)rva);
        host->log(host->context, "info", MOD_ID, message);
        if (host->capability_register(host->context, "example.game-info", MOD_ID) != 0)
            host->log(host->context, "warn", MOD_ID, "capability already registered");
    } else {
        snprintf(message, sizeof message, "resource path hash mismatch: got %llx expected %llx",
                 (unsigned long long)hash, (unsigned long long)GAME_RESOURCE_PATH_FNV);
        host->log(host->context, "error", MOD_ID, message);
    }
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
    NULL, 0, /* dependencies */
    mod_initialize,
    mod_shutdown,
    NULL, 0, /* conflicts */
    "Game info",
    "Verifies a fingerprinted game value through the ABI 5 image API.",
    "HYPRLODR",
    HYPRLODR_PLUGIN_FLAG_DIAGNOSTIC,
};

HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version) {
    return host_abi_version == HYPRLODR_ABI_VERSION ? &descriptor : NULL;
}
