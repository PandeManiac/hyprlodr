/* HYPRLODR sample mod z.foundation: creates shared state and a capability. */
#include "hyprlodr/plugin.h"

#define MOD_ID "z.foundation"
#define MOD_VERSION "1.0.0"

static int mod_initialize(const hyprlodr_host_api *host) {
    if (host->state_set(host->context, "suite.foundation", MOD_VERSION) != 0) return -1;
    if (host->capability_register(host->context, "test.foundation", MOD_ID) != 0) return -1;
    host->log(host->context, "info", MOD_ID, "created shared test state and registered test.foundation");
    return 0;
}

static void mod_shutdown(const hyprlodr_host_api *host) {
    host->state_remove(host->context, "suite.foundation");
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
    "Foundation",
    "Provides shared test state and the test.foundation capability.",
    "HYPRLODR",
    HYPRLODR_PLUGIN_FLAG_DIAGNOSTIC,
};

HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version) {
    return host_abi_version == HYPRLODR_ABI_VERSION ? &descriptor : NULL;
}
