/*
 * HYPRLODR hand hook (example.hand-hook).
 *
 * The engine does not resolve glDrawElements through an import slot. It calls
 * through its own dispatch table, an array of driver pointers, at RVA 0x3d9ac8.
 * This mod saves the pointer that is there, replaces it with a shim, and calls
 * the original for every draw. Two adjacent 1,188-index draws are the
 * first-person arm (docs/RE_HAND.md); the shim recognises them by count, type
 * and mode.
 *
 * The first mode is suppression: force the hand draw's count to zero while
 * every other draw is forwarded untouched. It is the reversible proof that the
 * persistent hook is live, and it is the foundation for substituting the
 * authored arm geometry in the same place.
 *
 * The mode is read once from <state_dir>/hand-mode.txt:
 *   "suppress"  hide the original arm
 *   anything else (or no file)  pass every draw through unchanged
 *
 * This example is Windows-only: it needs the live game image and the host's
 * ABI 6 write gate. On a host without a verified image it initializes inert.
 */
#include "hyprlodr/plugin.h"
#include "hypr_win32.h"

#include <stdio.h>
#include <string.h>

#define MOD_ID "example.hand-hook"
#define MOD_VERSION "0.1.0"

/* The engine's draw-table slot, from docs/RE_HAND.md. The host only lets a mod
 * touch the image when build_matches() is true, and this value is tied to the
 * fingerprinted build. */
#define HAND_DISPATCH_RVA 0x3d9ac8ull

#define HAND_INDEX_COUNT 1188
#define GL_TRIANGLES 4u
/* The live draw declares 5125 = GL_UNSIGNED_INT, and the 4,752-byte element
 * buffer is one run of 1,188 32-bit indices (not two 16-bit runs). */
#define GL_UNSIGNED_INT 0x1405u
#define GL_UNSIGNED_SHORT 0x1403u
#define GL_CURRENT_PROGRAM 0x8B8Du

typedef void (WINAPI *draw_elements_fn)(unsigned mode, int count, unsigned type,
                                        const void *indices);

static const hyprlodr_host_api *g_host;
static draw_elements_fn g_original_draw;
static unsigned g_mode; /* 0 pass-through, 1 suppress */
static unsigned long g_hand_draws;
static int g_active;

static void log_message(const char *level, const char *message) {
    if (g_host && g_host->log) g_host->log(g_host->context, level, MOD_ID, message);
}

static const char *state_path(const hyprlodr_host_api *host, const char *name,
                              char *buffer, size_t capacity) {
    if (!HYPRLODR_HOST_API_HAS(host, state_dir) || !host->state_dir) return NULL;
    const char *directory = host->state_dir(host->context);
    if (!directory || !*directory) return NULL;
    int written = snprintf(buffer, capacity, "%s/%s", directory, name);
    if (written < 0 || (size_t)written >= capacity) return NULL;
    return buffer;
}

/* The minimal Windows C runtime has no memcmp, so compare prefixes directly. */
static int has_prefix(const char *text, const char *prefix) {
    while (*prefix) {
        if (*text++ != *prefix++) return 0;
    }
    return 1;
}

/* Read the mode before anything is patched. Parsed by hand rather than with a
 * scanf the minimal Windows runtime does not provide. */
static unsigned read_mode(const hyprlodr_host_api *host) {
    char path[1024];
    if (!state_path(host, "hand-mode.txt", path, sizeof path)) return 0;
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    char word[32];
    unsigned mode = 0;
    if (fgets(word, (int)sizeof word, file)) {
        if (has_prefix(word, "suppress")) mode = 1;
    }
    fclose(file);
    return mode;
}

/* The shim runs on the render thread inside the engine's own draw call. It
 * must not call the host logger or any other code that can take a loader lock:
 * the first version did, and the game died on the first gameplay hand draw. It
 * only counts, optionally zeroes the count, and forwards. The count is logged
 * from shutdown(), which runs on the loader's thread. */
static void WINAPI hand_draw_shim(unsigned mode, int count, unsigned type,
                                  const void *indices) {
    if (g_active && mode == GL_TRIANGLES && count == HAND_INDEX_COUNT &&
        (type == GL_UNSIGNED_INT || type == GL_UNSIGNED_SHORT)) {
        ++g_hand_draws;
        if (g_mode == 1) count = 0;
    }
    g_original_draw(mode, count, type, indices);
}

static int mod_initialize(const hyprlodr_host_api *host) {
    g_host = host;

    if (!HYPRLODR_HOST_API_HAS(host, image_write) || !host->image_write ||
        !host->image_read) {
        log_message("warn", "host has no image access; hand hook inactive");
        return 0;
    }
    if (!host->build_matches(host->context)) {
        log_message("warn", "build fingerprint does not match; hand hook inactive");
        return 0;
    }

    void *original = NULL;
    if (host->image_read(host->context, HAND_DISPATCH_RVA, &original,
                         sizeof original) != 0) {
        log_message("error", "cannot read the draw dispatch slot");
        return -1;
    }
    if (!original) {
        log_message("error", "draw dispatch slot is empty");
        return -1;
    }
    g_original_draw = (draw_elements_fn)original;
    g_mode = read_mode(host);

    void *replacement = (void *)&hand_draw_shim;
    if (host->image_write(host->context, HAND_DISPATCH_RVA, &replacement,
                          sizeof replacement) != 0) {
        log_message("error", "cannot patch the draw dispatch slot");
        return -1;
    }

    void *check = NULL;
    if (host->image_read(host->context, HAND_DISPATCH_RVA, &check, sizeof check) != 0 ||
        check != replacement) {
        log_message("error", "dispatch patch did not take");
        return -1;
    }

    g_active = 1;
    char message[192];
    snprintf(message, sizeof message,
             "dispatch hook installed at rva 0x3d9ac8 (original=0x%lx mode=%s)",
             (unsigned long)(size_t)original, g_mode == 1 ? "suppress" : "pass-through");
    log_message("info", message);
    return 0;
}

static void mod_shutdown(const hyprlodr_host_api *host) {
    g_active = 0;
    size_t restored = 0;
    if (HYPRLODR_HOST_API_HAS(host, image_restore) && host->image_restore)
        restored = host->image_restore(host->context);

    char message[192];
    snprintf(message, sizeof message,
             "hook removed after %lu hand draws; %lu image byte(s) restored",
             g_hand_draws, (unsigned long)restored);
    host->log(host->context, "info", MOD_ID, message);
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
    "Hand hook",
    "Patches the engine's GL draw dispatch table to locate and suppress the "
    "first-person hand draw.",
    "HYPRLODR",
    0,
};

HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version) {
    return host_abi_version == HYPRLODR_ABI_VERSION ? &descriptor : NULL;
}
