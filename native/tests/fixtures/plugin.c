/*
 * Configurable test fixture mod. One source file, compiled many times with
 * different -D macros, produces the error-case fixtures the loader suite needs.
 * Everything here goes through the real public ABI; no callbacks are simulated.
 */
#include "hyprlodr/plugin.h"

#include <stdio.h>
#include <string.h>

#ifndef FIX_ID
#  error "FIX_ID must be defined"
#endif
#ifndef FIX_VERSION
#  define FIX_VERSION "1.0.0"
#endif
#ifndef FIX_ABI
#  define FIX_ABI HYPRLODR_ABI_VERSION
#endif

#if defined(__GNUC__)
#  define FIX_UNUSED __attribute__((unused))
#else
#  define FIX_UNUSED
#endif

static const char *const dependencies[] = {
#ifdef FIX_DEP1
    FIX_DEP1,
#endif
#ifdef FIX_DEP2
    FIX_DEP2,
#endif
#ifdef FIX_DEP3
    FIX_DEP3,
#endif
    NULL,
};

static const char *const conflicts[] = {
#ifdef FIX_CONFLICT1
    FIX_CONFLICT1,
#endif
    NULL,
};

static size_t list_count(const char *const *list, size_t capacity) {
    size_t count = 0;
    while (count < capacity && list[count]) ++count;
    return count;
}

static FIX_UNUSED int mod_initialize(const hyprlodr_host_api *host) {
    host->log(host->context, "info", FIX_ID, "fixture initialize");

#ifdef FIX_STATE_KEY
    if (host->state_set(host->context, FIX_STATE_KEY, FIX_STATE_VALUE) != 0) return -1;
#endif
#ifdef FIX_STATE_FILE
    /* ABI 3: write a real file into the directory the host reserved for us. */
    const char *directory = HYPRLODR_HOST_API_HAS(host, state_dir) && host->state_dir
                                ? host->state_dir(host->context)
                                : NULL;
    if (!directory || !*directory) {
        host->log(host->context, "error", FIX_ID, "state_dir unavailable");
        return -1;
    }
    char state_path[1024];
    snprintf(state_path, sizeof state_path, "%s/%s", directory, FIX_STATE_FILE);
    FILE *state_file = fopen(state_path, "w");
    if (!state_file) {
        host->log(host->context, "error", FIX_ID, "cannot create state file");
        return -1;
    }
    fputs(FIX_ID, state_file);
    fclose(state_file);
    host->log(host->context, "info", FIX_ID, state_path);
#endif
#ifdef FIX_REQUIRE_STATE
    const char *value = host->state_get(host->context, FIX_REQUIRE_STATE);
    if (!value || strcmp(value, FIX_REQUIRE_VALUE) != 0) {
        host->log(host->context, "error", FIX_ID, "required state missing or unexpected");
        return -1;
    }
#endif
#ifdef FIX_CAP
    if (host->capability_register(host->context, FIX_CAP, FIX_ID) != 0) return -1;
#endif
#ifdef FIX_REQUIRE_CAP
    if (!host->capability_has(host->context, FIX_REQUIRE_CAP)) {
        host->log(host->context, "error", FIX_ID, "required capability missing");
        return -1;
    }
#endif
#ifdef FIX_FAIL
    host->log(host->context, "warn", FIX_ID, "intentional initialization failure");
    return -1;
#endif

    host->log(host->context, "info", FIX_ID, "fixture initialized");
    return 0;
}

static FIX_UNUSED void mod_shutdown(const hyprlodr_host_api *host) {
    host->log(host->context, "info", FIX_ID, "fixture shutdown");
}

static FIX_UNUSED const hyprlodr_plugin_descriptor descriptor = {
#ifdef FIX_DESC_SIZE
    (uint32_t)(FIX_DESC_SIZE),
#else
    (uint32_t)sizeof(hyprlodr_plugin_descriptor),
#endif
#ifdef FIX_DESC_ABI
    (uint32_t)(FIX_DESC_ABI),
#else
    (uint32_t)HYPRLODR_ABI_VERSION,
#endif
    FIX_ID,
    FIX_VERSION,
    dependencies,
    sizeof dependencies / sizeof dependencies[0] - 1,
    mod_initialize,
    mod_shutdown,
    conflicts,
    sizeof conflicts / sizeof conflicts[0] - 1,
    "Fixture " FIX_ID,
    "Generated loader test fixture.",
    "HYPRLODR tests",
    HYPRLODR_PLUGIN_FLAG_DIAGNOSTIC,
};

#ifndef FIX_NO_ENTRY
HYPR_EXPORT const hyprlodr_plugin_descriptor *
hyprlodr_plugin_query(uint32_t host_abi_version) {
#ifdef FIX_BAD_DESCRIPTOR
    (void)host_abi_version;
    return NULL;
#elif defined(FIX_ALWAYS_ANSWER)
    /* Deliberately answer any handshake so the host's descriptor-level ABI
     * check is what rejects this fixture. */
    (void)host_abi_version;
    return &descriptor;
#else
    return host_abi_version == (uint32_t)(FIX_ABI) ? &descriptor : NULL;
#endif
}
#endif

/* Keep list_count referenced in builds that use no dependency macros. */
static FIX_UNUSED size_t fixture_dependency_count(void) {
    return list_count(dependencies, sizeof dependencies / sizeof dependencies[0]);
}
