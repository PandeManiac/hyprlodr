/* Managed interception for an exact, verified x64 CALL rel32 instruction.
 * Installation is only supported at an externally guaranteed quiescent point.
 * There is deliberately no physical unpatch/free operation: disable routing,
 * then retain the callsite, relay, state, and callback code until process exit. */
#ifndef HYPRLODR_CALLSITE_HOOK_H
#define HYPRLODR_CALLSITE_HOOK_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    HYPRLODR_CALL_HOOK_OK = 0,
    HYPRLODR_CALL_HOOK_ALREADY = 1,
    HYPRLODR_CALL_HOOK_BAD_ARGUMENT = -1,
    HYPRLODR_CALL_HOOK_NOT_QUIESCENT = -2,
    HYPRLODR_CALL_HOOK_BYTES_MISMATCH = -3,
    HYPRLODR_CALL_HOOK_TARGET_MISMATCH = -4,
    HYPRLODR_CALL_HOOK_OUT_OF_RANGE = -5,
    HYPRLODR_CALL_HOOK_RELAY_FAILED = -6,
    HYPRLODR_CALL_HOOK_PROTECT_FAILED = -7,
    HYPRLODR_CALL_HOOK_FLUSH_FAILED = -8,
    HYPRLODR_CALL_HOOK_ALREADY_DIFFERENT = -9
} hyprlodr_call_hook_status;

/* This token is a caller assertion, not a thread-suspension mechanism.
 * Windows loader initialization can supply a process-start phase only when the
 * embedding host has separately established that no DLL/TLS callback can
 * reenter the target. A fixture may supply it only while all callers are
 * stopped. */
typedef enum {
    HYPRLODR_CALL_HOOK_INSTALL_QUIESCENT = 0x48595131u
} hyprlodr_call_hook_install_mode;

typedef void *(*hyprlodr_call_hook_alloc_near_fn)(void *context, uintptr_t site);
typedef int (*hyprlodr_call_hook_protect_fn)(void *context, void *address,
                                             size_t length, uint32_t protection,
                                             uint32_t *old_protection);
typedef int (*hyprlodr_call_hook_flush_fn)(void *context, const void *address,
                                           size_t length);

/* Optional platform operations allow deterministic failure-path tests. A NULL
 * ops pointer selects the real Windows APIs. Allocated relays are never freed. */
typedef struct {
    void *context;
    hyprlodr_call_hook_alloc_near_fn alloc_near;
    hyprlodr_call_hook_protect_fn protect;
    hyprlodr_call_hook_flush_fn flush;
} hyprlodr_call_hook_ops;

typedef struct {
    volatile long enabled;
    int installed;
    int uses_relay;
    void *site;
    void *original_target;
    void *replacement;
    void *relay;
    unsigned char original_bytes[5];
} hyprlodr_call_hook;

#define HYPRLODR_CALL_HOOK_INITIALIZER { 0 }

/* expected_call must be the exact five bytes, including E8 and rel32.
 * expected_original_target may be NULL when the call bytes themselves are the
 * full expected-target check. Install writes five bytes and is NOT safe while
 * any thread can execute site. The site must not straddle a 4 KiB page. */
hyprlodr_call_hook_status hyprlodr_call_hook_install(
    hyprlodr_call_hook *hook, void *site, const unsigned char expected_call[5],
    void *expected_original_target, void *replacement,
    hyprlodr_call_hook_install_mode mode, const hyprlodr_call_hook_ops *ops);

/* Atomic logical state only. The replacement callback must read this state and
 * call original_target exactly once when disabled. In-flight callbacks remain
 * valid because neither callback nor relay is reclaimed. */
void hyprlodr_call_hook_set_enabled(hyprlodr_call_hook *hook, int enabled);
int hyprlodr_call_hook_is_enabled(const hyprlodr_call_hook *hook);

#endif
