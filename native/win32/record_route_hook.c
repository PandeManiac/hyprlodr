#include "record_route_hook.h"
#include "startup_diag.h"

#include <string.h>

typedef uintptr_t (*game_loader_fn)(void *context, void *record,
                                    const void *name_object, uint32_t flags);

typedef struct {
    game_loader_fn function;
    void *game_context;
    uint32_t original_tag;
    uint32_t original_offset;
    uint32_t original_size;
} original_call;

static hyprlodr_game_route_binding *volatile g_binding;
static void *volatile g_original_target;

static uint32_t record_u32(const void *record, size_t offset) {
    uint32_t value = 0;
    if (record) memcpy(&value, (const unsigned char *)record + offset, sizeof value);
    return value;
}

static uint32_t binding_gate_state(const hyprlodr_game_route_binding *binding) {
    volatile long *gate = binding ?
        __atomic_load_n(&binding->activation_gate, __ATOMIC_ACQUIRE) : NULL;
    return gate ? (uint32_t)__atomic_load_n(gate, __ATOMIC_ACQUIRE) : 0xffffffffu;
}

static uint32_t hand2_diag_flags(int hand2, uint32_t tag, uint32_t offset,
                                 uint32_t size,
                                 const hyprlodr_game_route_binding *binding,
                                 int routed) {
    uint32_t flags = hand2 ? 1u : 0u;
    if (hand2 && tag == 1u && offset == 295758784u && size == 21972u) flags |= 2u;
    if (hand2 && tag == 2u) flags |= 4u;
    if (binding && __atomic_load_n(&binding->route, __ATOMIC_ACQUIRE)) flags |= 8u;
    if (binding && binding->hook && hyprlodr_call_hook_is_enabled(binding->hook)) flags |= 16u;
    if (routed) flags |= 32u;
    return flags;
}

static int game_name_is_hand2(const void *name_object, char output[6]) {
    const unsigned char *object = (const unsigned char *)name_object;
    uint64_t size = 0;
    uint64_t capacity = 0;
    const unsigned char *data;
    if (!object) return 0;
    memcpy(&size, object + 0x10u, sizeof size);
    memcpy(&capacity, object + 0x18u, sizeof capacity);
    if (size != 5u || capacity < size) return 0;
    if (capacity < 16u) {
        data = object;
    } else {
        uintptr_t pointer = 0;
        memcpy(&pointer, object, sizeof pointer);
        if (!pointer) return 0;
        data = (const unsigned char *)pointer;
    }
    if (data[0] != 'h' || data[1] != 'a' || data[2] != 'n' ||
        data[3] != 'd' || data[4] != '2') return 0;
    memcpy(output, "hand2", 5u);
    output[5] = '\0';
    return 1;
}

void hyprlodr_record_route_hook_bind(hyprlodr_game_route_binding *binding,
                                    hyprlodr_call_hook *hook,
                                    const hyprlodr_record_route *route) {
    if (!binding) return;
    binding->hook = hook;
    binding->activation_gate = NULL;
    __atomic_store_n(&binding->route, route, __ATOMIC_RELEASE);
    (void)__atomic_exchange_n(&g_binding, binding, __ATOMIC_RELEASE);
}

void hyprlodr_record_route_hook_set_activation_gate(
    hyprlodr_game_route_binding *binding, volatile long *gate_state) {
    if (binding)
        __atomic_store_n(&binding->activation_gate, gate_state, __ATOMIC_RELEASE);
}

void hyprlodr_record_route_hook_set_original_target(void *target) {
    __atomic_store_n(&g_original_target, target, __ATOMIC_RELEASE);
}

void hyprlodr_record_route_hook_publish_route(
    hyprlodr_game_route_binding *binding, const hyprlodr_record_route *route) {
    if (binding) __atomic_store_n(&binding->route, route, __ATOMIC_RELEASE);
}

static uintptr_t invoke_original(void *context, void *record,
                                 const void *name_object, uint32_t flags) {
    original_call *call = (original_call *)context;
    if (hyprlodr_startup_diag_enabled()) {
        char name[6];
        if (game_name_is_hand2(name_object, name)) {
            uint32_t tag = record_u32(record, HYPRLODR_RECORD_TAG_FIELD);
            uint32_t offset = record_u32(record, HYPRLODR_RECORD_OFFSET_FIELD);
            uint32_t size = record_u32(record, HYPRLODR_RECORD_SIZE_FIELD);
            hyprlodr_game_route_binding *binding =
                __atomic_load_n(&g_binding, __ATOMIC_ACQUIRE);
            hyprlodr_startup_diag_emit_once(HYPRLODR_DIAG_HAND2_LOADER_CALL,
                tag, offset, size, binding_gate_state(binding),
                hand2_diag_flags(1, tag, offset, size, binding, 0), 0u, 0u,
                "original loader entered");
            if (call->original_tag == 1u &&
                call->original_offset == 295758784u &&
                call->original_size == 21972u)
                hyprlodr_startup_diag_emit_once(HYPRLODR_DIAG_TARGET_LOADER_CALL,
                    tag, offset, size, binding_gate_state(binding),
                    hand2_diag_flags(1, tag, offset, size, binding, 0), 0u, 0u,
                    "original loader entered for verified tag-1 hand2");
        }
    }
    return call->function(call->game_context, record, name_object, flags);
}

uintptr_t hyprlodr_record_route_hook_call(void *context, void *record,
                                         const void *name_object, uint32_t flags) {
    hyprlodr_game_route_binding *binding =
        __atomic_load_n(&g_binding, __ATOMIC_ACQUIRE);
    game_loader_fn original;
    original_call call;
    char requested_name[6];
    const hyprlodr_record_route *route = NULL;
    uintptr_t result;
    void *original_target = __atomic_load_n(&g_original_target, __ATOMIC_ACQUIRE);
    int diag = hyprlodr_startup_diag_enabled();
    int hand2 = 0;
    int target_hand2 = 0;
    uint32_t original_offset = 0, original_size = 0, tag = 0;
    int route_can_run = binding && binding->hook &&
                        hyprlodr_call_hook_is_enabled(binding->hook);
    if (diag || route_can_run) {
        if (diag) {
            original_offset = record_u32(record, HYPRLODR_RECORD_OFFSET_FIELD);
            original_size = record_u32(record, HYPRLODR_RECORD_SIZE_FIELD);
            tag = record_u32(record, HYPRLODR_RECORD_TAG_FIELD);
            hyprlodr_startup_diag_emit_once(HYPRLODR_DIAG_FIRST_CALLSITE,
                tag, original_offset, original_size, binding_gate_state(binding),
                0u, 0u, 0u, "first intercepted loader callsite invocation");
        }
        hand2 = game_name_is_hand2(name_object, requested_name);
    }
    if (diag) {
        if (hand2) {
            hyprlodr_startup_diag_emit_once(HYPRLODR_DIAG_FIRST_HAND2,
                tag, original_offset, original_size, binding_gate_state(binding),
                hand2_diag_flags(hand2, tag, original_offset, original_size, binding, 0),
                0u, 0u, "first exact hand2 name request");
            if (tag == 1u && original_offset == 295758784u &&
                original_size == 21972u) {
                target_hand2 = 1;
                hyprlodr_startup_diag_emit_once(HYPRLODR_DIAG_FIRST_TARGET,
                    tag, original_offset, original_size, binding_gate_state(binding),
                    hand2_diag_flags(hand2, tag, original_offset, original_size,
                                     binding, 0),
                    0u, 0u, "verified original tag-1 hand2 resolver metadata");
            }
        }
    }
    if (binding && binding->hook && binding->hook->original_target)
        original_target = binding->hook->original_target;
    /* Installation publishes this target before modifying the call instruction.
     * A null value is therefore only possible if the wrapper was misused. */
    if (!original_target) return 0;
    original = (game_loader_fn)original_target;
    call.function = original;
    call.game_context = context;
    call.original_tag = tag;
    call.original_offset = original_offset;
    call.original_size = original_size;
    if (!binding || !binding->hook || !hyprlodr_call_hook_is_enabled(binding->hook)) {
        result = invoke_original(&call, record, name_object, flags);
        goto report_hand2;
    }
    volatile long *gate = __atomic_load_n(&binding->activation_gate, __ATOMIC_ACQUIRE);
    if (gate && __atomic_load_n(gate, __ATOMIC_ACQUIRE) != 1l) {
        result = invoke_original(&call, record, name_object, flags);
        goto report_hand2;
    }
    route = __atomic_load_n(&binding->route, __ATOMIC_ACQUIRE);
    if (!hand2 || !route) {
        result = invoke_original(&call, record, name_object, flags);
        goto report_hand2;
    }
    hyprlodr_route_status status = hyprlodr_record_route_invoke(
        record, HYPRLODR_GAME_RECORD_SIZE, requested_name, route,
        invoke_original, &call, name_object, flags, &result);
    if (status == HYPRLODR_ROUTE_BAD_ARGUMENT || status == HYPRLODR_ROUTE_BAD_MANIFEST) {
        result = invoke_original(&call, record, name_object, flags);
        status = HYPRLODR_ROUTE_PASSTHROUGH;
    }
    if (hand2) {
        uintptr_t object = 0;
        if (record) {
            memcpy(&object, (const unsigned char *)record + HYPRLODR_RECORD_OBJECT_FIELD,
                   sizeof object);
        }
        hyprlodr_startup_diag_emit_once(HYPRLODR_DIAG_HAND2_LOADER_RETURN,
            tag, original_offset, original_size, binding_gate_state(binding),
            hand2_diag_flags(hand2, tag, original_offset, original_size, binding,
                             status == HYPRLODR_ROUTE_APPLIED) | (object ? 64u : 0u),
            result, object, object ? "original loader returned parsed object" :
                                     "original loader returned without parsed object");
        if (target_hand2)
            hyprlodr_startup_diag_emit_once(HYPRLODR_DIAG_TARGET_LOADER_RETURN,
                tag, original_offset, original_size, binding_gate_state(binding),
                hand2_diag_flags(hand2, tag, original_offset, original_size, binding,
                                 status == HYPRLODR_ROUTE_APPLIED) | (object ? 64u : 0u),
                result, object, object ? "tag-1 hand2 returned parsed object" :
                                         "tag-1 hand2 returned without parsed object");
    }
    return result;

report_hand2:
    if (hand2) {
        uintptr_t object = 0;
        if (record) memcpy(&object,
            (const unsigned char *)record + HYPRLODR_RECORD_OBJECT_FIELD, sizeof object);
        hyprlodr_startup_diag_emit_once(HYPRLODR_DIAG_HAND2_LOADER_RETURN,
            tag, original_offset, original_size, binding_gate_state(binding),
            hand2_diag_flags(hand2, tag, original_offset, original_size, binding, 0) |
                (object ? 64u : 0u), result, object,
            object ? "original loader returned parsed object" :
                     "original loader returned without parsed object");
        if (target_hand2)
            hyprlodr_startup_diag_emit_once(HYPRLODR_DIAG_TARGET_LOADER_RETURN,
                tag, original_offset, original_size, binding_gate_state(binding),
                hand2_diag_flags(hand2, tag, original_offset, original_size, binding, 0) |
                    (object ? 64u : 0u), result, object,
                object ? "tag-1 hand2 returned parsed object" :
                         "tag-1 hand2 returned without parsed object");
    }
    return result;
}
