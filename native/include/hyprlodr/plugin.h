#ifndef HYPRLODR_PLUGIN_H
#define HYPRLODR_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  define HYPR_EXPORT __declspec(dllexport)
#else
#  define HYPR_EXPORT __attribute__((visibility("default")))
#endif

#define HYPRLODR_ABI_VERSION 1u
#define HYPRLODR_PLUGIN_ENTRY "hyprlodr_plugin_query"

typedef struct hyprlodr_host_api {
    uint32_t struct_size;
    uint32_t abi_version;
    void *context;
    void (*log)(void *context, const char *level, const char *mod_id, const char *message);
    int (*state_set)(void *context, const char *key, const char *value);
    const char *(*state_get)(void *context, const char *key);
    int (*capability_register)(void *context, const char *name, const char *provider_mod_id);
    int (*capability_has)(void *context, const char *name);
} hyprlodr_host_api;

typedef struct hyprlodr_plugin_descriptor {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *id;
    const char *version;
    const char *const *dependencies;
    size_t dependency_count;
    int (*initialize)(const hyprlodr_host_api *host);
    void (*shutdown)(const hyprlodr_host_api *host);
} hyprlodr_plugin_descriptor;

typedef const hyprlodr_plugin_descriptor *(*hyprlodr_query_fn)(uint32_t host_abi_version);

/* Export this symbol from each native plugin. Descriptor strings remain owned by the plugin. */
HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version);

#endif
