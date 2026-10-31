/* HYPRLODR sample mod m.overlay: reads the state a.palette produced. */
#include "hyprlodr/plugin.h"

#include <string.h>

#define MOD_ID "m.overlay"
#define MOD_VERSION "1.0.0"

static const char *const dependencies[] = {"a.palette"};

static int mod_initialize(const hyprlodr_host_api *host) {
    const char *palette = host->state_get(host->context, "suite.palette");
    if (!palette || strcmp(palette, "applied") != 0) return -1;
    if (!host->capability_has(host->context, "test.palette")) return -1;
    if (host->capability_register(host->context, "test.overlay", MOD_ID) != 0) return -1;
    host->log(host->context, "info", MOD_ID, "observed modified state and registered overlay capability");
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
    dependencies,
    1,
    mod_initialize,
    mod_shutdown,
    NULL,
    0,
    "Overlay",
    "Verifies palette state and publishes test.overlay.",
    "HYPRLODR",
    HYPRLODR_PLUGIN_FLAG_DIAGNOSTIC,
};

HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version) {
    return host_abi_version == HYPRLODR_ABI_VERSION ? &descriptor : NULL;
}
