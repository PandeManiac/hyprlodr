/*
 * HYPRLODR public plugin ABI.
 *
 * A mod is a native shared library that exports exactly one symbol,
 * HYPRLODR_PLUGIN_ENTRY, returning a pointer to a static descriptor.
 *
 * Compatibility: the host passes its ABI version to the query function. A mod
 * must return NULL when it does not support that version. The host also checks
 * descriptor->struct_size so that newer descriptors can be extended by
 * appending fields only.
 *
 * Native mods are trusted code running inside the host process. They can crash
 * the process or do anything the game can do. See docs/SECURITY.md.
 */
#ifndef HYPRLODR_PLUGIN_H
#define HYPRLODR_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  define HYPR_EXPORT __declspec(dllexport)
#else
#  define HYPR_EXPORT __attribute__((visibility("default")))
#endif

/* Bumped to 5: the host API gained read-only access to the verified game image,
 * so a mod can inspect the running game instead of only running beside it. ABI 4
 * added file_redirect(). Earlier ABIs are rejected: mods are recompiled against
 * this header. */
#define HYPRLODR_ABI_VERSION 5u
#define HYPRLODR_PLUGIN_ENTRY "hyprlodr_plugin_query"

/* True when the descriptor is large enough to contain `field`. */
#define HYPRLODR_DESCRIPTOR_HAS(d, field) \
    ((d)->struct_size >= offsetof(hyprlodr_plugin_descriptor, field) + sizeof((d)->field))

typedef struct hyprlodr_host_api {
    uint32_t struct_size;
    uint32_t abi_version;
    void *context;
    void (*log)(void *context, const char *level, const char *mod_id, const char *message);
    int (*state_set)(void *context, const char *key, const char *value);
    const char *(*state_get)(void *context, const char *key);
    int (*capability_register)(void *context, const char *name, const char *provider_mod_id);
    int (*capability_has)(void *context, const char *name);
    /* --- ABI 2 additions --- */
    int (*state_remove)(void *context, const char *key);
    int (*is_loaded)(void *context, const char *mod_id);
    /* --- ABI 3 additions --- */
    /* Absolute path to a directory reserved for this mod, created before
     * initialize() runs. NULL when the host was started without a state root,
     * so check before writing. The path stays valid until shutdown(). */
    const char *(*state_dir)(void *context);
    /* --- ABI 4 additions --- */
    /* Ask the host to open `replacement_path` instead whenever the game opens a
     * file whose path ends with `suffix`, matched case-insensitively and with
     * \\ and / treated as the same separator. The suffix must start at a path
     * separator, so "asset/asset" matches "<game>\\asset\\asset" but not
     * "<game>\\xasset/asset".
     *
     * Registration is only meaningful while initialize() runs: the host
     * publishes the registry once every mod has initialized, and a redirect
     * registered by a mod that fails is rolled back with its other changes.
     * Hosts without file control (the synthetic test host, for example) may
     * return non-zero; a mod should treat that as "override unavailable". */
    int (*file_redirect)(void *context, const char *suffix, const char *replacement_path);
    /* --- ABI 5 additions --- */
    /* Bytes of the running game image that may be read. 0 when the host has no
     * image to offer (the synthetic test host, for example). */
    size_t (*image_size)(void *context);
    /* Non-zero when the image is the exact build the verifier describes. Every
     * read is refused while this is 0, so a mod can never act on addresses that
     * came from a different executable. */
    int (*build_matches)(void *context);
    /* Copy `length` bytes at `offset` in the game image into `destination`.
     * Returns 0 on success; non-zero when refused: no image, unknown build,
     * zero length, or a range that is not entirely inside the image. */
    int (*image_read)(void *context, uint64_t offset, void *destination, size_t length);
} hyprlodr_host_api;

/* True when the host API is large enough to contain `field`. */
#define HYPRLODR_HOST_API_HAS(api, field) \
    ((api)->struct_size >= offsetof(hyprlodr_host_api, field) + sizeof((api)->field))

/* Dependency strings may carry a version constraint after '@':
 *   "z.foundation"          any version
 *   "z.foundation@>=1.2.0"  comparison: >=, <=, >, <, =
 *   "z.foundation@^1.2.0"   same major version
 *   "z.foundation@~1.2.0"   same major+minor version
 */
typedef struct hyprlodr_plugin_descriptor {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *id;
    const char *version;
    const char *const *dependencies;
    size_t dependency_count;
    int (*initialize)(const hyprlodr_host_api *host);
    void (*shutdown)(const hyprlodr_host_api *host);
    /* --- ABI 2 additions --- */
    const char *const *conflicts;
    size_t conflict_count;
    const char *display_name;
    const char *description;
    const char *author;
    uint32_t flags;
} hyprlodr_plugin_descriptor;

#define HYPRLODR_PLUGIN_FLAG_NONE 0u
/* Diagnostic mods are expected to be inert: the host only logs them. */
#define HYPRLODR_PLUGIN_FLAG_DIAGNOSTIC (1u << 0)

typedef const hyprlodr_plugin_descriptor *(*hyprlodr_query_fn)(uint32_t host_abi_version);

#ifdef __cplusplus
extern "C" {
#endif

/* Export this symbol from each native plugin. Descriptor strings remain owned
 * by the plugin and must stay valid for the lifetime of the process. */
HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version);

#ifdef __cplusplus
}
#endif

#endif /* HYPRLODR_PLUGIN_H */
