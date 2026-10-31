/* HYPRLODR sample mod 00.final: verifies every prerequisite is present. */
#include "hyprlodr/plugin.h"

#include <string.h>

#define MOD_ID "00.final"
#define MOD_VERSION "1.0.0"

static const char *const dependencies[] = {"m.overlay", "b.audio"};

static int mod_initialize(const hyprlodr_host_api *host) {
    const char *foundation = host->state_get(host->context, "suite.foundation");
    const char *palette = host->state_get(host->context, "suite.palette");
    if (!foundation || !palette || strcmp(palette, "applied") != 0) return -1;
    if (!host->capability_has(host->context, "test.foundation")) return -1;
    if (!host->capability_has(host->context, "test.audio")) return -1;
    if (!host->capability_has(host->context, "test.overlay")) return -1;
    if (!host->is_loaded(host->context, "z.foundation")) return -1;
    if (!host->is_loaded(host->context, "m.overlay")) return -1;
    if (host->state_set(host->context, "suite.status", "ready") != 0) return -1;
    host->log(host->context, "info", MOD_ID, "confirmed all prerequisites and shared state");
    return 0;
}

static void mod_shutdown(const hyprlodr_host_api *host) {
    host->state_remove(host->context, "suite.status");
    host->log(host->context, "info", MOD_ID, "shutdown complete");
}

static const hyprlodr_plugin_descriptor descriptor = {
    (uint32_t)sizeof descriptor,
    HYPRLODR_ABI_VERSION,
    MOD_ID,
    MOD_VERSION,
    dependencies,
    2,
    mod_initialize,
    mod_shutdown,
    NULL,
    0,
    "Final checks",
    "Asserts that every prerequisite capability and state value is present.",
    "HYPRLODR",
    HYPRLODR_PLUGIN_FLAG_DIAGNOSTIC,
};

HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version) {
    return host_abi_version == HYPRLODR_ABI_VERSION ? &descriptor : NULL;
}
