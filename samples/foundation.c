#include "hyprlodr/plugin.h"
static int init(const hyprlodr_host_api *h) {
    if (h->state_set(h->context, "shared", "foundation") || h->capability_register(h->context, "test.foundation", "z.foundation")) return -1;
    h->log(h->context, "info", "z.foundation", "created shared test state"); return 0;
}
static void stop(const hyprlodr_host_api *h) { h->log(h->context, "info", "z.foundation", "shutdown complete"); }
static const hyprlodr_plugin_descriptor d = {sizeof(d), HYPRLODR_ABI_VERSION, "z.foundation", "1.0.0", NULL, 0, init, stop};
const hyprlodr_plugin_descriptor *hyprlodr_plugin_query(uint32_t v) { return v == HYPRLODR_ABI_VERSION ? &d : NULL; }
