/*
 * HYPRLODR worked example: example.title-replacement.
 *
 * The replacement is mostly data: a package ships `assets/title-mask.raw` and its manifest
 * installs it beside the game as `hyprlodr-title-mask.raw`, which is the file the proxy's GL
 * hook uploads into the game's own title texture. This mod is the part that runs in the game
 * and says whether that actually happened, so a user sees "replacement active" or
 * "no mask, original title" in <game>/hyprlodr-events.jsonl instead of guessing.
 *
 * It deliberately holds no game bytes and no RVAs: it only measures the mask's size. Writing
 * 853x480 RGBA is what the renderer needs, and a mismatch is worth a warning rather than a
 * failure, because the game still runs with its own title.
 *
 * Build it like the other examples: header on the include path, no extra libraries.
 */
#include "hyprlodr/plugin.h"

#include <stdio.h>
#include <string.h>

#define MOD_ID "example.title-replacement"
#define MOD_VERSION "1.0.0"

#define MASK_NAME "hyprlodr-title-mask.raw"
/* 853 x 480 RGBA - the texture the game's own title pass samples. */
#define MASK_BYTES (853ul * 480ul * 4ul)

static int game_directory(const hyprlodr_host_api *host, char *out, size_t capacity) {
    if (!HYPRLODR_HOST_API_HAS(host, state_dir) || !host->state_dir) return -1;
    const char *directory = host->state_dir(host->context);
    if (!directory || !*directory) return -1;

    /* The host reserves <game>/hyprlodr-state/<mod id>, so walking back two separators
     * gives the directory the game itself runs from. */
    size_t length = strlen(directory);
    int separators = 0;
    while (length > 0 && separators < 2) {
        --length;
        if (directory[length] == '/' || directory[length] == '\\') ++separators;
    }
    if (separators < 2 || length == 0 || length >= capacity) return -1;
    memcpy(out, directory, length);
    out[length] = '\0';
    return 0;
}

/* The minimal Windows CRT has no fseek/ftell pair worth relying on here, and the file is
 * under two megabytes, so it is counted as it is read. */
static unsigned long file_size(const char *path) {
    unsigned char block[4096];
    unsigned long total = 0;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    size_t got;
    while ((got = fread(block, 1, sizeof block, file)) > 0) total += (unsigned long)got;
    fclose(file);
    return total;
}

static int mod_initialize(const hyprlodr_host_api *host) {
    if (host->capability_register(host->context, "example.title-replacement", MOD_ID) != 0) return -1;

    char directory[1024];
    if (game_directory(host, directory, sizeof directory) != 0) {
        host->log(host->context, "warn", MOD_ID,
                  "no state directory; cannot look for the title mask");
        return 0;
    }

    char path[1100];
    int written = snprintf(path, sizeof path, "%s/%s", directory, MASK_NAME);
    if (written < 0 || (size_t)written >= sizeof path) {
        host->log(host->context, "warn", MOD_ID, "the mask path does not fit the buffer");
        return 0;
    }

    unsigned long size = file_size(path);
    if (size == MASK_BYTES) {
        host->log(host->context, "info", MOD_ID,
                  "title mask in place (853x480 RGBA); the game draws HYPR over LODR");
    } else if (size == 0) {
        host->log(host->context, "warn", MOD_ID,
                  "no title mask beside the game; the original title renders");
    } else {
        char message[160];
        snprintf(message, sizeof message, "title mask is %lu bytes, expected %lu",
                 size, (unsigned long)MASK_BYTES);
        host->log(host->context, "warn", MOD_ID, message);
    }
    return 0;
}

static void mod_shutdown(const hyprlodr_host_api *host) {
    host->log(host->context, "info", MOD_ID, "shutdown; the game keeps its own title texture");
}

static const char *const dependencies[] = {
    NULL,
};

static const hyprlodr_plugin_descriptor descriptor = {
    (uint32_t)sizeof descriptor,
    HYPRLODR_ABI_VERSION,
    MOD_ID,
    MOD_VERSION,
    dependencies,
    sizeof dependencies / sizeof dependencies[0] - 1,
    mod_initialize,
    mod_shutdown,
    NULL,
    0,
    "Title replacement",
    "Reports whether the HYPR / LODR title mask is installed beside the game.",
    "HYPRLODR",
    0,
};

HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version) {
    return host_abi_version == HYPRLODR_ABI_VERSION ? &descriptor : NULL;
}
