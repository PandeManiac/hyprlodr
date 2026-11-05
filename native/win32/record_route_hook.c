#include "record_route_hook.h"

#include <string.h>

typedef uintptr_t (*game_loader_fn)(void *context, void *record,
                                    const void *name_object, uint32_t flags);

typedef struct {
    game_loader_fn function;
    void *game_context;
} original_call;

static hyprlodr_game_route_binding *volatile g_binding;
static void *volatile g_original_target;

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
    if (binding && binding->hook && binding->hook->original_target)
        original_target = binding->hook->original_target;
    /* Installation publishes this target before modifying the call instruction.
     * A null value is therefore only possible if the wrapper was misused. */
    if (!original_target) return 0;
    original = (game_loader_fn)original_target;
    call.function = original;
    call.game_context = context;
    if (!binding || !binding->hook || !hyprlodr_call_hook_is_enabled(binding->hook))
        return original(context, record, name_object, flags);
    volatile long *gate = __atomic_load_n(&binding->activation_gate, __ATOMIC_ACQUIRE);
    if (gate && __atomic_load_n(gate, __ATOMIC_ACQUIRE) != 1l)
        return original(context, record, name_object, flags);
    route = __atomic_load_n(&binding->route, __ATOMIC_ACQUIRE);
    if (!game_name_is_hand2(name_object, requested_name) || !route)
        return original(context, record, name_object, flags);
    hyprlodr_route_status status = hyprlodr_record_route_invoke(
        record, HYPRLODR_GAME_RECORD_SIZE, requested_name, route,
        invoke_original, &call, name_object, flags, &result);
    if (status == HYPRLODR_ROUTE_BAD_ARGUMENT || status == HYPRLODR_ROUTE_BAD_MANIFEST)
        return original(context, record, name_object, flags);
    return result;
}
