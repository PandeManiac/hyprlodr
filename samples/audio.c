#include "hyprlodr/plugin.h"
static const char *deps[] = {"z.foundation"};
static int init(const hyprlodr_host_api *h) {
    if (!h->capability_has(h->context, "test.foundation") || h->capability_register(h->context, "test.audio", "b.audio")) return -1;
    h->log(h->context, "info", "b.audio", "registered independent audio test capability"); return 0;
}
static void stop(const hyprlodr_host_api *h) { h->log(h->context, "info", "b.audio", "shutdown complete"); }
static const hyprlodr_plugin_descriptor d = {sizeof(d), HYPRLODR_ABI_VERSION, "b.audio", "1.0.0", deps, 1, init, stop};
const hyprlodr_plugin_descriptor *hyprlodr_plugin_query(uint32_t v) { return v == HYPRLODR_ABI_VERSION ? &d : NULL; }
