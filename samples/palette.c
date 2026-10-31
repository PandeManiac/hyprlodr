/* HYPRLODR sample mod a.palette: depends on z.foundation, extends its state. */
#include "hyprlodr/plugin.h"

#include <string.h>

#define MOD_ID "a.palette"
#define MOD_VERSION "1.0.0"

static const char *const dependencies[] = {"z.foundation@^1.0.0"};

static int mod_initialize(const hyprlodr_host_api *host) {
    const char *foundation = host->state_get(host->context, "suite.foundation");
    if (!foundation || strcmp(foundation, "1.0.0") != 0) return -1;
    if (!host->is_loaded(host->context, "z.foundation")) return -1;
    if (host->state_set(host->context, "suite.palette", "applied") != 0) return -1;
    if (host->capability_register(host->context, "test.palette", MOD_ID) != 0) return -1;
    host->log(host->context, "info", MOD_ID, "validated foundation state and applied palette");
    return 0;
}

static void mod_shutdown(const hyprlodr_host_api *host) {
    host->state_remove(host->context, "suite.palette");
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
    "Palette",
    "Consumes foundation state and publishes test.palette.",
    "HYPRLODR",
    HYPRLODR_PLUGIN_FLAG_DIAGNOSTIC,
};

HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version) {
    return host_abi_version == HYPRLODR_ABI_VERSION ? &descriptor : NULL;
}
