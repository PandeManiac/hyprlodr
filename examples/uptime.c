/*
 * HYPRLODR worked example: example.uptime.
 *
 * A complete, real mod rather than a test fixture. It demonstrates the four
 * things a mod author usually needs first:
 *
 *   1. a versioned dependency on another mod (z.foundation@^1.0.0), so the
 *      loader orders it after that mod;
 *   2. persistent per-mod storage through state_dir(), which the host reserves
 *      before initialize() runs;
 *   3. publishing a capability other mods can check;
 *   4. logging, which lands in <game>/hyprlodr-events.jsonl next to the
 *      loader's own events.
 *
 * What it actually does: counts how many times the game has started with it
 * enabled, and records the version that last ran. That is enough to prove the
 * state directory works across runs without touching any game code.
 *
 * Build it exactly like the samples: header on the include path, no extra
 * libraries. See docs/MOD_API.md.
 */
#include "hyprlodr/plugin.h"

#include <stdio.h>
#include <string.h>

#define MOD_ID "example.uptime"
#define MOD_VERSION "1.0.0"

#define COUNTER_FILE "launches.txt"
#define LAST_RUN_FILE "last-run.txt"

/* A hard dependency on the foundation sample, with a version constraint. */
static const char *const dependencies[] = {
    "z.foundation@^1.0.0",
    NULL,
};

/* Join the mod's state directory with a file name. Returns NULL if the host
 * has no state root (ABI 2, or a host without --state-root) or the path would
 * not fit. */
static const char *state_path(const hyprlodr_host_api *host, const char *name,
                              char *buffer, size_t capacity) {
    if (!HYPRLODR_HOST_API_HAS(host, state_dir) || !host->state_dir) return NULL;
    const char *directory = host->state_dir(host->context);
    if (!directory || !*directory) return NULL;
    int written = snprintf(buffer, capacity, "%s/%s", directory, name);
    if (written < 0 || (size_t)written >= capacity) return NULL;
    return buffer;
}

/* Parsed by hand: the Windows target's minimal CRT has no fscanf, and this is
 * the one number the example needs. */
static unsigned long read_counter(const char *path) {
    char buffer[32];
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    if (!fgets(buffer, (int)sizeof buffer, file)) {
        fclose(file);
        return 0;
    }
    fclose(file);

    unsigned long value = 0;
    size_t index = 0;
    while (buffer[index] >= '0' && buffer[index] <= '9') {
        value = value * 10ul + (unsigned long)(buffer[index] - '0');
        ++index;
    }
    return value;
}

static int mod_initialize(const hyprlodr_host_api *host) {
    char path[1024];

    const char *counter_path = state_path(host, COUNTER_FILE, path, sizeof path);
    if (!counter_path) {
        host->log(host->context, "error", MOD_ID, "no state directory; cannot count launches");
        return -1;
    }

    /* The dependency is ordered before us, so its state is already published.
     * Its absence is worth a warning, not a failure: we only use it for the
     * log message. */
    const char *foundation = host->state_get(host->context, "suite.foundation");
    if (!foundation) {
        host->log(host->context, "warn", MOD_ID, "z.foundation published no version");
    }

    unsigned long launches = read_counter(counter_path) + 1;
    FILE *counter = fopen(counter_path, "w");
    if (!counter) {
        host->log(host->context, "error", MOD_ID, "cannot write the launch counter");
        return -1;
    }
    fprintf(counter, "%lu", launches);
    fclose(counter);

    const char *last_run_path = state_path(host, LAST_RUN_FILE, path, sizeof path);
    if (!last_run_path) return -1;
    FILE *last_run = fopen(last_run_path, "w");
    if (!last_run) {
        host->log(host->context, "error", MOD_ID, "cannot write the last-run marker");
        return -1;
    }
    fprintf(last_run, "%s\n", MOD_VERSION);
    fclose(last_run);

    if (host->capability_register(host->context, "example.uptime", MOD_ID) != 0) return -1;

    char message[128];
    snprintf(message, sizeof message, "launch %lu of %s recorded", launches, MOD_VERSION);
    host->log(host->context, "info", MOD_ID, message);
    if (foundation) host->log(host->context, "info", MOD_ID, "dependency z.foundation is present");
    return 0;
}

static void mod_shutdown(const hyprlodr_host_api *host) {
    host->log(host->context, "info", MOD_ID, "shutdown; launch count stays on disk");
}

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
    "Uptime",
    "Counts how many times the game started with this mod enabled.",
    "HYPRLODR",
    0,
};

HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version) {
    return host_abi_version == HYPRLODR_ABI_VERSION ? &descriptor : NULL;
}
