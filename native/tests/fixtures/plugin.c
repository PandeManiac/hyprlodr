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
#ifdef FIX_IMAGE_READ
    /* ABI 5: read the running image through the host. The suite compares the
     * logged value with the bytes it supplied, so a wrong offset or a silently
     * refused read cannot pass. */
    if (!HYPRLODR_HOST_API_HAS(host, image_read) || !host->image_read) {
        host->log(host->context, "error", FIX_ID, "image_read unavailable");
        return -1;
    }
    {
        unsigned char bytes[8];
        for (int index = 0; index < 8; ++index) bytes[index] = 0;
        if (host->image_read(host->context, (uint64_t)FIX_IMAGE_READ, bytes, sizeof bytes) != 0) {
            host->log(host->context, "warn", FIX_ID, "image read refused");
        } else {
            unsigned long long value = 0;
            for (int index = 0; index < 8; ++index)
                value = (value << 8) | (unsigned long long)bytes[index];
            char message[64];
            snprintf(message, sizeof message, "image %016llx", value);
            host->log(host->context, "info", FIX_ID, message);
        }
    }
#endif
#ifdef FIX_IMAGE_WRITE
    /* ABI 6: patch eight bytes at a fixed offset, then read them back. The suite
     * compares the logged value with what it wrote and, through --image-out,
     * checks that the host put the original bytes back after shutdown. */
    if (!HYPRLODR_HOST_API_HAS(host, image_write) || !host->image_write) {
        host->log(host->context, "error", FIX_ID, "image_write unavailable");
        return -1;
    }
    {
        unsigned char payload[8];
        for (int index = 0; index < 8; ++index) payload[index] = (unsigned char)(0xa0 + index);
        if (host->image_write(host->context, (uint64_t)FIX_IMAGE_WRITE, payload,
                              sizeof payload) != 0) {
            host->log(host->context, "warn", FIX_ID, "image write refused");
        } else {
            unsigned char bytes[8];
            for (int index = 0; index < 8; ++index) bytes[index] = 0;
            if (host->image_read(host->context, (uint64_t)FIX_IMAGE_WRITE, bytes, sizeof bytes) != 0) {
                host->log(host->context, "warn", FIX_ID, "image readback refused");
            } else {
                unsigned long long value = 0;
                for (int index = 0; index < 8; ++index)
                    value = (value << 8) | (unsigned long long)bytes[index];
                char message[64];
                snprintf(message, sizeof message, "image write ok %016llx", value);
                host->log(host->context, "info", FIX_ID, message);
            }
        }
    }
#endif
#ifdef FIX_STATE_REMOVE
    if (host->state_remove(host->context, FIX_STATE_REMOVE) != 0) {
        host->log(host->context, "error", FIX_ID, "state_remove failed");
        return -1;
    }
#endif
#ifdef FIX_STATE_COUNTER
    /* Persistence across processes: increment a counter kept in our own state
     * directory. Two host runs must observe 1 then 2. */
    const char *counter_dir = HYPRLODR_HOST_API_HAS(host, state_dir) && host->state_dir
                                  ? host->state_dir(host->context)
                                  : NULL;
    if (!counter_dir || !*counter_dir) {
        host->log(host->context, "error", FIX_ID, "state_dir unavailable");
        return -1;
    }
    char counter_path[1024];
    snprintf(counter_path, sizeof counter_path, "%s/%s", counter_dir, FIX_STATE_COUNTER);
    int counter_value = 0;
    FILE *counter_file = fopen(counter_path, "r");
    if (counter_file) {
        if (fscanf(counter_file, "%d", &counter_value) != 1) counter_value = 0;
        fclose(counter_file);
    }
    counter_value += 1;
    counter_file = fopen(counter_path, "w");
    if (!counter_file) {
        host->log(host->context, "error", FIX_ID, "cannot write state counter");
        return -1;
    }
    fprintf(counter_file, "%d", counter_value);
    fclose(counter_file);
    {
        char counter_message[64];
        snprintf(counter_message, sizeof counter_message, "state counter %d", counter_value);
        host->log(host->context, "info", FIX_ID, counter_message);
    }
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
#ifdef FIX_REDIRECT_FROM
    /* ABI 4: point a game path at a file this mod owns. Registration happens
     * while initialize() runs; the host publishes it after every mod started. */
    if (!HYPRLODR_HOST_API_HAS(host, file_redirect) || !host->file_redirect) {
        host->log(host->context, "error", FIX_ID, "file_redirect unavailable");
        return -1;
    }
    if (host->file_redirect(host->context, FIX_REDIRECT_FROM, FIX_REDIRECT_TO) != 0) {
        host->log(host->context, "error", FIX_ID, "file_redirect refused");
        return -1;
    }
    host->log(host->context, "info", FIX_ID, "registered a file redirect");
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
#ifdef FIX_SHUTDOWN_REQUIRE_STATE
    /* Runs after a failed mod was rolled back, so this observes whether the
     * loader really restored the state the failed mod touched. */
    const char *value = host->state_get(host->context, FIX_SHUTDOWN_REQUIRE_STATE);
    if (!value || strcmp(value, FIX_SHUTDOWN_REQUIRE_VALUE) != 0) {
        host->log(host->context, "error", FIX_ID, "state was not restored after rollback");
        return;
    }
#endif
#ifdef FIX_SHUTDOWN_REQUIRE_ABSENT
    if (host->state_get(host->context, FIX_SHUTDOWN_REQUIRE_ABSENT) != NULL) {
        host->log(host->context, "error", FIX_ID, "state written by the failed mod survived rollback");
        return;
    }
#endif
#if defined(FIX_SHUTDOWN_REQUIRE_STATE) || defined(FIX_SHUTDOWN_REQUIRE_ABSENT)
    host->log(host->context, "info", FIX_ID, "shutdown state verified");
#endif
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
