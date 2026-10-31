#include "hyprlodr/plugin.h"
static const char *deps[] = {"z.foundation"};
static int init(const hyprlodr_host_api *h) {
    const char *s = h->state_get(h->context, "shared");
    if (!s || h->state_set(h->context, "shared", "foundation+palette")) return -1;
    h->log(h->context, "info", "a.palette", "validated foundation state and applied palette"); return 0;
}
static void stop(const hyprlodr_host_api *h) { h->log(h->context, "info", "a.palette", "shutdown complete"); }
static const hyprlodr_plugin_descriptor d = {sizeof(d), HYPRLODR_ABI_VERSION, "a.palette", "1.0.0", deps, 1, init, stop};
const hyprlodr_plugin_descriptor *hyprlodr_plugin_query(uint32_t v) { return v == HYPRLODR_ABI_VERSION ? &d : NULL; }
