#include "callsite_hook.h"
#include "hypr_win32.h"

#include <string.h>

#define HOOK_CALL_SIZE 5u
#define HOOK_RELAY_SIZE 13u
#define HOOK_PAGE_SIZE 4096u
#define HOOK_ALLOC_GRANULARITY 0x10000u
#define HOOK_RELAY_MAX_DISTANCE 0x7fff0000u

static void *system_alloc_near(void *context, uintptr_t site) {
    (void)context;
    uintptr_t base = site & ~(uintptr_t)(HOOK_ALLOC_GRANULARITY - 1u);
    for (uintptr_t distance = HOOK_ALLOC_GRANULARITY;
         distance <= HOOK_RELAY_MAX_DISTANCE; distance += HOOK_ALLOC_GRANULARITY) {
        uintptr_t candidates[2];
        candidates[0] = base <= ~(uintptr_t)0 - distance ? base + distance : 0u;
        candidates[1] = base >= distance ? base - distance : 0u;
        for (unsigned index = 0; index < 2u; ++index) {
            if (candidates[index] == 0u) continue;
            void *allocation = VirtualAlloc((void *)candidates[index], HOOK_PAGE_SIZE,
                                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (allocation) return allocation;
        }
    }
    return NULL;
}

static int system_protect(void *context, void *address, size_t length,
                          uint32_t protection, uint32_t *old_protection) {
    (void)context;
    DWORD old_value = 0;
    if (!VirtualProtect(address, length, protection, &old_value)) return 0;
    *old_protection = old_value;
    return 1;
}

static int system_flush(void *context, const void *address, size_t length) {
    (void)context;
    return FlushInstructionCache(GetCurrentProcess(), address, length) != 0;
}

static int rel32_value(uintptr_t next_instruction, uintptr_t destination, int32_t *out) {
    if (destination >= next_instruction) {
        uintptr_t distance = destination - next_instruction;
        if (distance > 0x7fffffffu) return 0;
        *out = (int32_t)distance;
        return 1;
    }
    uintptr_t distance = next_instruction - destination;
    if (distance > 0x80000000u) return 0;
    if (distance == 0x80000000u) *out = (-2147483647 - 1);
    else *out = -(int32_t)distance;
    return 1;
}

static uintptr_t decode_target(const unsigned char call[5], uintptr_t site) {
    int32_t displacement;
    memcpy(&displacement, call + 1u, sizeof displacement);
    if (displacement >= 0) return site + 5u + (uintptr_t)displacement;
    return site + 5u - (uintptr_t)(-(int64_t)displacement);
}

static void encode_relay(unsigned char relay[HOOK_RELAY_SIZE], uintptr_t destination) {
    relay[0] = 0x49u; /* mov r10, imm64; R10 is volatile and not an argument register. */
    relay[1] = 0xbau;
    memcpy(relay + 2u, &destination, sizeof destination);
    relay[10] = 0x41u; /* jmp r10 */
    relay[11] = 0xffu;
    relay[12] = 0xe2u;
}

static int valid_ops(const hyprlodr_call_hook_ops *ops) {
    return !ops || (ops->alloc_near && ops->protect && ops->flush);
}

static int bytes_equal(const unsigned char *left, const unsigned char *right,
                       size_t length) {
    for (size_t index = 0; index < length; ++index)
        if (left[index] != right[index]) return 0;
    return 1;
}

hyprlodr_call_hook_status hyprlodr_call_hook_install(
    hyprlodr_call_hook *hook, void *site, const unsigned char expected_call[5],
    void *expected_original_target, void *replacement,
    hyprlodr_call_hook_install_mode mode, const hyprlodr_call_hook_ops *ops) {
    if (!hook || !site || !expected_call || !replacement || !valid_ops(ops))
        return HYPRLODR_CALL_HOOK_BAD_ARGUMENT;
    if (mode != HYPRLODR_CALL_HOOK_INSTALL_QUIESCENT)
        return HYPRLODR_CALL_HOOK_NOT_QUIESCENT;

    if (hook->installed) {
        if (hook->site == site && hook->replacement == replacement &&
            (!expected_original_target || hook->original_target == expected_original_target))
            return HYPRLODR_CALL_HOOK_ALREADY;
        return HYPRLODR_CALL_HOOK_ALREADY_DIFFERENT;
    }
    if (((uintptr_t)site & (HOOK_PAGE_SIZE - 1u)) > HOOK_PAGE_SIZE - HOOK_CALL_SIZE)
        return HYPRLODR_CALL_HOOK_BAD_ARGUMENT;

    unsigned char *call = (unsigned char *)site;
    if (expected_call[0] != 0xe8u || !bytes_equal(call, expected_call, HOOK_CALL_SIZE))
        return HYPRLODR_CALL_HOOK_BYTES_MISMATCH;
    unsigned char saved_call[HOOK_CALL_SIZE];
    memcpy(saved_call, expected_call, HOOK_CALL_SIZE);
    uintptr_t original = decode_target(saved_call, (uintptr_t)site);
    if (expected_original_target && original != (uintptr_t)expected_original_target)
        return HYPRLODR_CALL_HOOK_TARGET_MISMATCH;

    hyprlodr_call_hook_alloc_near_fn alloc_near = ops ? ops->alloc_near : system_alloc_near;
    hyprlodr_call_hook_protect_fn protect = ops ? ops->protect : system_protect;
    hyprlodr_call_hook_flush_fn flush = ops ? ops->flush : system_flush;
    void *ops_context = ops ? ops->context : NULL;
    uintptr_t patch_target = (uintptr_t)replacement;
    void *relay = NULL;
    int32_t displacement = 0;
    int uses_relay = 0;
    uintptr_t next = (uintptr_t)site + HOOK_CALL_SIZE;

    if (!rel32_value(next, patch_target, &displacement)) {
        relay = alloc_near(ops_context, (uintptr_t)site);
        if (!relay) return HYPRLODR_CALL_HOOK_RELAY_FAILED;
        if (!rel32_value(next, (uintptr_t)relay, &displacement))
            return HYPRLODR_CALL_HOOK_OUT_OF_RANGE;
        unsigned char relay_code[HOOK_RELAY_SIZE];
        encode_relay(relay_code, patch_target);
        memcpy(relay, relay_code, sizeof relay_code);
        uint32_t relay_old_protection = 0;
        if (!protect(ops_context, relay, HOOK_PAGE_SIZE, PAGE_EXECUTE_READ,
                     &relay_old_protection))
            return HYPRLODR_CALL_HOOK_PROTECT_FAILED;
        if (!flush(ops_context, relay, sizeof relay_code))
            return HYPRLODR_CALL_HOOK_FLUSH_FAILED;
        patch_target = (uintptr_t)relay;
        uses_relay = 1;
    }

    unsigned char replacement_call[5];
    replacement_call[0] = 0xe8u;
    if (!rel32_value(next, patch_target, &displacement))
        return HYPRLODR_CALL_HOOK_OUT_OF_RANGE;
    memcpy(replacement_call + 1u, &displacement, sizeof displacement);

    uint32_t old_protection = 0;
    if (!protect(ops_context, site, HOOK_CALL_SIZE, PAGE_EXECUTE_READWRITE,
                 &old_protection))
        return HYPRLODR_CALL_HOOK_PROTECT_FAILED;
    memcpy(call, replacement_call, HOOK_CALL_SIZE);
    if (!flush(ops_context, site, HOOK_CALL_SIZE)) {
        memcpy(call, saved_call, HOOK_CALL_SIZE);
        (void)flush(ops_context, site, HOOK_CALL_SIZE);
        uint32_t ignored = 0;
        (void)protect(ops_context, site, HOOK_CALL_SIZE, old_protection, &ignored);
        return HYPRLODR_CALL_HOOK_FLUSH_FAILED;
    }
    uint32_t ignored = 0;
    if (!protect(ops_context, site, HOOK_CALL_SIZE, old_protection, &ignored)) {
        /* Windows leaves the region's current protection unchanged on failure;
         * the quiescent contract lets us safely put the original bytes back. */
        memcpy(call, saved_call, HOOK_CALL_SIZE);
        (void)flush(ops_context, site, HOOK_CALL_SIZE);
        (void)protect(ops_context, site, HOOK_CALL_SIZE, old_protection, &ignored);
        return HYPRLODR_CALL_HOOK_PROTECT_FAILED;
    }

    memcpy(hook->original_bytes, saved_call, HOOK_CALL_SIZE);
    hook->site = site;
    hook->original_target = (void *)original;
    hook->replacement = replacement;
    hook->relay = relay;
    hook->uses_relay = uses_relay;
    hook->enabled = 0;
    hook->installed = 1;
    return HYPRLODR_CALL_HOOK_OK;
}

void hyprlodr_call_hook_set_enabled(hyprlodr_call_hook *hook, int enabled) {
    if (hook && hook->installed)
        __atomic_store_n(&hook->enabled, enabled ? 1l : 0l, __ATOMIC_RELEASE);
}

int hyprlodr_call_hook_is_enabled(const hyprlodr_call_hook *hook) {
    if (!hook || !hook->installed) return 0;
    return __atomic_load_n(&hook->enabled, __ATOMIC_ACQUIRE) != 0;
}
