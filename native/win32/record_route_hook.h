/* HYPER DEMON-specific bridge between the verified resource-loader call ABI
 * and the generic CALL rel32 hook. The game name is an MSVC x64 basic_string
 * object (size +0x10, capacity +0x18, inline storage/pointer +0), not char *. */
#ifndef HYPRLODR_RECORD_ROUTE_HOOK_H
#define HYPRLODR_RECORD_ROUTE_HOOK_H

#include "callsite_hook.h"
#include "hyprlodr/record_route.h"

typedef struct {
    hyprlodr_call_hook *hook;
    const hyprlodr_record_route * volatile route;
    /* Optional startup gate. When bound, route substitution is allowed only
     * after this atomic word reaches the READY value (1). */
    volatile long *activation_gate;
} hyprlodr_game_route_binding;

/* Bind immutable state before installing the callsite. binding and route must
 * remain alive until process exit because the replacement remains reachable. */
void hyprlodr_record_route_hook_bind(hyprlodr_game_route_binding *binding,
                                    hyprlodr_call_hook *hook,
                                    const hyprlodr_record_route *route);

/* Must be published before installing the callsite. This allows an early,
 * dormant callback to forward safely before any route binding is populated. */
void hyprlodr_record_route_hook_set_original_target(void *target);
void hyprlodr_record_route_hook_publish_route(
    hyprlodr_game_route_binding *binding, const hyprlodr_record_route *route);
void hyprlodr_record_route_hook_set_activation_gate(
    hyprlodr_game_route_binding *binding, volatile long *gate_state);

/* Exact Windows x64 loader ABI: ctx, 0x58-byte descriptor, MSVC string*, flags. */
uintptr_t hyprlodr_record_route_hook_call(void *context, void *record,
                                         const void *name_object, uint32_t flags);

#endif
