/* HYPRLODR sample mod b.audio: independent capability on top of foundation. */
#include "hyprlodr/plugin.h"

#define MOD_ID "b.audio"
#define MOD_VERSION "1.0.0"

static const char *const dependencies[] = {"z.foundation"};

static int mod_initialize(const hyprlodr_host_api *host) {
    if (!host->capability_has(host->context, "test.foundation")) return -1;
    if (host->capability_register(host->context, "test.audio", MOD_ID) != 0) return -1;
    host->log(host->context, "info", MOD_ID, "registered independent audio test capability");
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
    "Audio",
    "Registers test.audio without touching shared state.",
    "HYPRLODR",
    HYPRLODR_PLUGIN_FLAG_DIAGNOSTIC,
};

HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version) {
    return host_abi_version == HYPRLODR_ABI_VERSION ? &descriptor : NULL;
}
