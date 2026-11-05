#include "callsite_hook.h"
#include "record_route_hook.h"
#include "hypr_win32.h"

#include <stdio.h>
#include <string.h>

typedef uintptr_t (*call_thunk_fn)(void *, void *, const void *, uint32_t);

typedef struct {
    uint32_t marker;
    int reenter;
    int depth;
} game_context;

typedef struct {
    unsigned char bytes[32];
} msvc_string;

static volatile LONG g_original_calls;
static volatile LONG g_error_count;
static volatile LONG g_block;
static volatile LONG g_entered;
static volatile LONG g_release;
static const void *volatile g_last_name_object;
static call_thunk_fn g_thunk;
static hyprlodr_call_hook g_hook = HYPRLODR_CALL_HOOK_INITIALIZER;
static hyprlodr_game_route_binding g_binding;

static int check(int condition, const char *message) {
    if (condition) return 1;
    printf("callsite fixture: FAIL: %s\n", message);
    return 0;
}

static int bytes_equal(const unsigned char *left, const unsigned char *right,
                       size_t length) {
    for (size_t index = 0; index < length; ++index)
        if (left[index] != right[index]) return 0;
    return 1;
}

static LONG atomic_increment(volatile LONG *value) {
    return __atomic_add_fetch(value, 1l, __ATOMIC_SEQ_CST);
}

static LONG atomic_load(volatile LONG *value) {
    return __atomic_load_n(value, __ATOMIC_SEQ_CST);
}

static uint32_t get_u32(const void *buffer, size_t offset) {
    uint32_t value;
    memcpy(&value, (const unsigned char *)buffer + offset, sizeof value);
    return value;
}

static uintptr_t get_uptr(const void *buffer, size_t offset) {
    uintptr_t value;
    memcpy(&value, (const unsigned char *)buffer + offset, sizeof value);
    return value;
}

static void set_u32(void *buffer, size_t offset, uint32_t value) {
    memcpy((unsigned char *)buffer + offset, &value, sizeof value);
}

static void set_uptr(void *buffer, size_t offset, uintptr_t value) {
    memcpy((unsigned char *)buffer + offset, &value, sizeof value);
}

static int page_is_execute_read(const void *address) {
    MEMORY_BASIC_INFORMATION64A info;
    memset(&info, 0, sizeof info);
    return VirtualQuery(address, &info, sizeof info) == sizeof info &&
           (info.Protect & 0xffu) == PAGE_EXECUTE_READ;
}

static int make_rel32(uintptr_t next, uintptr_t destination, int32_t *value) {
    if (destination >= next) {
        uintptr_t distance = destination - next;
        if (distance > 0x7fffffffu) return 0;
        *value = (int32_t)distance;
    } else {
        uintptr_t distance = next - destination;
        if (distance > 0x80000000u) return 0;
        *value = distance == 0x80000000u ? (-2147483647 - 1) : -(int32_t)distance;
    }
    return 1;
}

static void *alloc_near(uintptr_t anchor) {
    uintptr_t base = anchor & ~(uintptr_t)0xffffu;
    for (uintptr_t distance = 0x10000u; distance < 0x40000000u; distance += 0x10000u) {
        void *a = VirtualAlloc((void *)(base + distance), 4096u,
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (a) return a;
        if (base > distance) {
            void *b = VirtualAlloc((void *)(base - distance), 4096u,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (b) return b;
        }
    }
    return NULL;
}

static uintptr_t original_loader(void *opaque, void *record,
                                 const void *name_object, uint32_t flags) {
    game_context *context = (game_context *)opaque;
    uint32_t offset = get_u32(record, HYPRLODR_RECORD_OFFSET_FIELD);
    uint32_t size = get_u32(record, HYPRLODR_RECORD_SIZE_FIELD);
    __atomic_store_n(&g_last_name_object, name_object, __ATOMIC_SEQ_CST);
    atomic_increment(&g_original_calls);
    if (!((offset == 295758784u && size == 21972u) ||
          (offset == 306794369u && size == 22056u)))
        atomic_increment(&g_error_count);
    if (atomic_load(&g_block)) {
        __atomic_store_n(&g_entered, 1l, __ATOMIC_SEQ_CST);
        while (!atomic_load(&g_release)) Sleep(0u);
    }
    if (context && context->reenter && context->depth == 0) {
        unsigned char nested_record[HYPRLODR_GAME_RECORD_SIZE] = {0};
        msvc_string nested_name;
        game_context nested_context = {context->marker, 0, 1};
        memset(&nested_name, 0, sizeof nested_name);
        memcpy(nested_name.bytes, "hand2", 5u);
        set_uptr(&nested_name, 0x10u, 5u);
        set_uptr(&nested_name, 0x18u, 15u);
        set_u32(nested_record, 0u, 295758784u);
        set_u32(nested_record, 4u, 21972u);
        set_u32(nested_record, HYPRLODR_RECORD_TAG_FIELD, 1u);
        context->depth++;
        (void)g_thunk(&nested_context, nested_record, &nested_name, flags);
        context->depth--;
        if (get_u32(nested_record, 0u) != 295758784u ||
            get_u32(nested_record, 4u) != 21972u)
            atomic_increment(&g_error_count);
    }
    set_uptr(record, HYPRLODR_RECORD_OBJECT_FIELD, (uintptr_t)0x123456789abcdef0ull);
    if (flags == 0xdeadbeefu) return (uintptr_t)0xbad0bad0u;
    return ((uintptr_t)context->marker << 32u) | (uintptr_t)flags;
}

static uintptr_t near_replacement(void *context, void *record,
                                  const void *name_object, uint32_t flags) {
    return original_loader(context, record, name_object, flags) ^ (uintptr_t)0x55aa55aau;
}

static void *make_far_bridge(void *destination) {
    uintptr_t preferred = (uintptr_t)0x5000000000ull;
    unsigned char *code = (unsigned char *)VirtualAlloc((void *)preferred, 4096u,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!code) code = (unsigned char *)VirtualAlloc(NULL, 4096u,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!code) return NULL;
    code[0] = 0x49u; code[1] = 0xbau;
    uintptr_t target = (uintptr_t)destination;
    memcpy(code + 2u, &target, sizeof target);
    code[10] = 0x41u; code[11] = 0xffu; code[12] = 0xe2u;
    DWORD old = 0;
    if (!VirtualProtect(code, 4096u, PAGE_EXECUTE_READ, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), code, 13u)) return NULL;
    return code;
}

static unsigned char *make_call_thunk(void *destination, unsigned char **callsite) {
    unsigned char *code = (unsigned char *)alloc_near((uintptr_t)destination);
    int32_t relative;
    if (!code) return NULL;
    /* Windows x64 entry alignment (RSP % 16 == 8): reserve shadow space and
     * align before calling. Incoming RCX/RDX/R8/R9D are passed unchanged. */
    code[0] = 0x48u; code[1] = 0x83u; code[2] = 0xecu; code[3] = 0x28u;
    code[4] = 0x48u; code[5] = 0xb8u;
    uintptr_t target = (uintptr_t)destination;
    memcpy(code + 6u, &target, sizeof target);
    code[14] = 0xe8u;
    if (!make_rel32((uintptr_t)(code + 19u), (uintptr_t)destination, &relative)) return NULL;
    memcpy(code + 15u, &relative, sizeof relative);
    code[19] = 0x48u; code[20] = 0x83u; code[21] = 0xc4u; code[22] = 0x28u;
    code[23] = 0xc3u;
    DWORD old = 0;
    if (!VirtualProtect(code, 4096u, PAGE_EXECUTE_READ, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), code, 24u)) return NULL;
    *callsite = code + 14u;
    return code;
}

static msvc_string make_name(const char *name) {
    msvc_string result;
    size_t length = strlen(name);
    memset(&result, 0, sizeof result);
    memcpy(result.bytes, name, length < 15u ? length : 15u);
    set_uptr(&result, 0x10u, length);
    set_uptr(&result, 0x18u, 15u);
    return result;
}

static msvc_string make_long_name(const char *name, const char **storage) {
    msvc_string result;
    size_t length = strlen(name);
    memset(&result, 0, sizeof result);
    *storage = name;
    uintptr_t pointer = (uintptr_t)*storage;
    memcpy(result.bytes, &pointer, sizeof pointer);
    set_uptr(&result, 0x10u, length);
    set_uptr(&result, 0x18u, 31u);
    return result;
}

static void init_record(unsigned char *record, uint32_t tag) {
    memset(record, 0xa5, HYPRLODR_GAME_RECORD_SIZE);
    set_u32(record, 0u, 295758784u);
    set_u32(record, 4u, 21972u);
    set_u32(record, HYPRLODR_RECORD_TAG_FIELD, tag);
    set_uptr(record, HYPRLODR_RECORD_OBJECT_FIELD, 0u);
}

typedef struct {
    volatile LONG fail_site_protect;
    void *site;
    volatile LONG fail_site_flush;
} fault_context;

static void *fault_alloc_near(void *opaque, uintptr_t anchor) {
    (void)opaque;
    return alloc_near(anchor);
}

static void *reject_alloc_near(void *opaque, uintptr_t anchor) {
    (void)opaque;
    (void)anchor;
    return NULL;
}

static int fault_protect(void *opaque, void *address, size_t length,
                         uint32_t protection, uint32_t *old_protection) {
    fault_context *fault = (fault_context *)opaque;
    if (address == fault->site &&
        __atomic_exchange_n(&fault->fail_site_protect, 0l, __ATOMIC_SEQ_CST)) return 0;
    DWORD old = 0;
    if (!VirtualProtect(address, length, protection, &old)) return 0;
    *old_protection = old;
    return 1;
}

static int fault_flush(void *opaque, const void *address, size_t length) {
    fault_context *fault = (fault_context *)opaque;
    if (fault && address == fault->site &&
        __atomic_exchange_n(&fault->fail_site_flush, 0l, __ATOMIC_SEQ_CST)) return 0;
    return FlushInstructionCache(GetCurrentProcess(), address, length) != 0;
}

static DWORD WINAPI concurrent_worker(void *opaque) {
    uintptr_t id = (uintptr_t)opaque;
    for (unsigned i = 0; i < 10000u; ++i) {
        unsigned char record[HYPRLODR_GAME_RECORD_SIZE];
        msvc_string name = make_name("hand2");
        game_context context = {(uint32_t)id, 0, 0};
        init_record(record, 1u);
        (void)g_thunk(&context, record, &name, i);
        if (get_u32(record, 0u) != 295758784u || get_u32(record, 4u) != 21972u)
            atomic_increment(&g_error_count);
    }
    return 0;
}

typedef struct {
    game_context context;
    unsigned char record[HYPRLODR_GAME_RECORD_SIZE];
    msvc_string name;
} inflight_call;

static DWORD WINAPI inflight_worker(void *opaque) {
    inflight_call *call = (inflight_call *)opaque;
    (void)g_thunk(&call->context, call->record, &call->name, 99u);
    return 0;
}

int main(int argc, char **argv) {
    unsigned char *callsite = NULL;
    unsigned char *code = make_call_thunk((void *)original_loader, &callsite);
    if (!check(code != NULL, "could not create synthetic call instruction")) return 1;
    g_thunk = (call_thunk_fn)code;

    hyprlodr_record_route route = {
        "hand2", 295758784u, 21972u, 1u, 306794369u, 22056u, 306816425u
    };
    static const unsigned expected_vertices = 239u;
    static const unsigned expected_indices = 1191u;
    if (!check(expected_vertices == 239u && expected_indices == 1191u &&
               route.replacement_size == 22056u, "replacement fixture metadata changed")) return 1;
    hyprlodr_record_route_hook_bind(&g_binding, &g_hook, &route);

    game_context context = {0x1234u, 0, 0};
    msvc_string hand2 = make_name("hand2");
    unsigned char record[HYPRLODR_GAME_RECORD_SIZE];
    unsigned char global_record[HYPRLODR_GAME_RECORD_SIZE];
    init_record(global_record, 1u);
    init_record(record, 1u);
    uintptr_t original_result = g_thunk(&context, record, &hand2, 0x55u);
    if (!check(original_result == (((uintptr_t)0x1234u << 32u) | 0x55u),
               "unhooked original return mismatch") ||
        !check(atomic_load(&g_original_calls) == 1 &&
               __atomic_load_n(&g_last_name_object, __ATOMIC_SEQ_CST) == &hand2,
               "original call count, context, or name argument mismatch")) return 1;

    void *far_bridge = make_far_bridge((void *)hyprlodr_record_route_hook_call);
    if (!check(far_bridge != NULL, "could not allocate far replacement bridge")) return 1;
    int32_t displacement = 0;
    if (!make_rel32((uintptr_t)(callsite + 5u), (uintptr_t)far_bridge, &displacement)) {
        hyprlodr_call_hook quiescence_probe = HYPRLODR_CALL_HOOK_INITIALIZER;
        if (!check(hyprlodr_call_hook_install(&quiescence_probe, callsite, callsite,
                  (void *)original_loader, far_bridge,
                  (hyprlodr_call_hook_install_mode)0, NULL) ==
                  HYPRLODR_CALL_HOOK_NOT_QUIESCENT,
                  "installer accepted a non-quiescent mode")) return 1;
        hyprlodr_call_hook_status status = hyprlodr_call_hook_install(
            &g_hook, callsite, callsite, (void *)original_loader, far_bridge,
            HYPRLODR_CALL_HOOK_INSTALL_QUIESCENT, NULL);
        if (!check(status == HYPRLODR_CALL_HOOK_OK && g_hook.uses_relay && g_hook.relay,
                   "out-of-range replacement did not use a near relay")) return 1;
    } else {
        printf("callsite fixture: far bridge unexpectedly rel32 reachable\n");
        return 1;
    }
    if (!check(hyprlodr_call_hook_install(&g_hook, callsite, callsite,
              (void *)original_loader, far_bridge,
              HYPRLODR_CALL_HOOK_INSTALL_QUIESCENT, NULL) == HYPRLODR_CALL_HOOK_ALREADY,
              "same install was not idempotent")) return 1;
    hyprlodr_call_hook_set_enabled(&g_hook, 1);
    init_record(record, 1u);
    uintptr_t routed = g_thunk(&context, record, &hand2, 0x66u);
    if (!check(routed == (((uintptr_t)0x1234u << 32u) | 0x66u) &&
               atomic_load(&g_original_calls) == 2 &&
               get_u32(record, 0u) == route.original_offset &&
               get_u32(record, 4u) == route.original_size &&
               get_uptr(record, HYPRLODR_RECORD_OBJECT_FIELD) ==
                    (uintptr_t)0x123456789abcdef0ull,
               "tag-1 route/return/metadata restoration failed")) return 1;
    if (!check(get_u32(global_record, 0u) == route.original_offset &&
               get_u32(global_record, 4u) == route.original_size,
               "global resolver source record changed")) return 1;
    init_record(record, 1u);
    uintptr_t failed_loader_result = g_thunk(&context, record, &hand2, 0xdeadbeefu);
    if (!check(failed_loader_result == (uintptr_t)0xbad0bad0u &&
               get_u32(record, 0u) == route.original_offset &&
               get_u32(record, 4u) == route.original_size,
               "loader failure return did not restore request metadata")) return 1;
    init_record(record, 2u);
    (void)g_thunk(&context, record, &hand2, 7u);
    if (!check(get_u32(record, 0u) == route.original_offset &&
               get_u32(record, 4u) == route.original_size,
               "tag-2 same-name request was routed")) return 1;
    const char *long_text = NULL;
    msvc_string other = make_long_name("hand2left", &long_text);
    init_record(record, 1u);
    (void)g_thunk(&context, record, &other, 8u);
    if (!check(get_u32(record, 0u) == route.original_offset,
               "unrelated resource was routed")) return 1;

    hyprlodr_call_hook_set_enabled(&g_hook, 0);
    init_record(record, 1u);
    (void)g_thunk(&context, record, &hand2, 9u);
    if (!check(get_u32(record, 0u) == route.original_offset &&
               g_hook.relay != NULL && !hyprlodr_call_hook_is_enabled(&g_hook),
               "logical disable or permanent relay lifetime failed")) return 1;

    /* Reentrant loader invocation: the original recursively enters the same
     * installed callsite with an independent stack descriptor. */
    hyprlodr_call_hook_set_enabled(&g_hook, 1);
    context.reenter = 1;
    init_record(record, 1u);
    (void)g_thunk(&context, record, &hand2, 10u);
    context.reenter = 0;
    if (!check(get_u32(record, 0u) == route.original_offset,
               "reentrant descriptor was not restored")) return 1;

    /* Active concurrent calls are allowed after install; disable only changes
     * an atomic flag and never rewrites code or frees reachable storage. */
    HANDLE threads[4];
    for (uintptr_t i = 0; i < 4u; ++i) {
        threads[i] = CreateThread(NULL, 0, concurrent_worker, (void *)(i + 1u), 0, NULL);
        if (!check(threads[i] != NULL, "CreateThread failed")) return 1;
    }
    for (unsigned i = 0; i < 100u; ++i) {
        hyprlodr_call_hook_set_enabled(&g_hook, (int)(i & 1u));
        Sleep(1u);
    }
    hyprlodr_call_hook_set_enabled(&g_hook, 0);
    for (unsigned i = 0; i < 4u; ++i) {
        (void)WaitForSingleObject(threads[i], 30000u);
        CloseHandle(threads[i]);
    }
    if (!check(atomic_load(&g_error_count) == 0,
               "concurrent/reentrant descriptor invariant failed")) return 1;

    /* In-flight invocation holds the permanent relay while logical shutdown
     * occurs. The call completes before this process exits; no memory is freed. */
    inflight_call active_call;
    __atomic_store_n(&g_block, 1l, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_entered, 0l, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_release, 0l, __ATOMIC_SEQ_CST);
    hyprlodr_call_hook_set_enabled(&g_hook, 1);
    active_call.context.marker = 5u;
    active_call.context.reenter = 0;
    active_call.context.depth = 0;
    active_call.name = make_name("hand2");
    init_record(active_call.record, 1u);
    HANDLE inflight = CreateThread(NULL, 0, inflight_worker, &active_call, 0, NULL);
    if (!check(inflight != NULL, "in-flight thread creation failed")) return 1;
    while (!atomic_load(&g_entered)) Sleep(0u);
    hyprlodr_call_hook_set_enabled(&g_hook, 0);
    __atomic_store_n(&g_release, 1l, __ATOMIC_SEQ_CST);
    (void)WaitForSingleObject(inflight, 10000u);
    CloseHandle(inflight);
    __atomic_store_n(&g_block, 0l, __ATOMIC_SEQ_CST);
    if (!check(g_hook.relay != NULL && g_hook.installed,
               "shutdown reclaimed an in-use relay or callback state")) return 1;
    if (!check(get_u32(active_call.record, 0u) == route.original_offset &&
               get_u32(active_call.record, 4u) == route.original_size,
               "in-flight call did not finish metadata restoration")) return 1;

    /* A mismatched instruction must fail without changing executable bytes. */
    unsigned char *bad_site = NULL;
    unsigned char *bad_code = make_call_thunk((void *)original_loader, &bad_site);
    if (!check(bad_code != NULL, "could not create mismatch fixture")) return 1;
    unsigned char bad_expected[5];
    memcpy(bad_expected, bad_site, 5u);
    bad_expected[0] = 0xe9u;
    hyprlodr_call_hook bad_hook = HYPRLODR_CALL_HOOK_INITIALIZER;
    if (!check(hyprlodr_call_hook_install(&bad_hook, bad_site, bad_expected,
              (void *)original_loader, (void *)near_replacement,
              HYPRLODR_CALL_HOOK_INSTALL_QUIESCENT, NULL) ==
              HYPRLODR_CALL_HOOK_BYTES_MISMATCH && bad_site[0] == 0xe8u,
              "expected-byte mismatch modified the callsite")) return 1;
    hyprlodr_call_hook_status target_mismatch = hyprlodr_call_hook_install(
        &bad_hook, bad_site, bad_site, (void *)(uintptr_t)1u,
        (void *)near_replacement, HYPRLODR_CALL_HOOK_INSTALL_QUIESCENT, NULL);
    if (!check(target_mismatch == HYPRLODR_CALL_HOOK_TARGET_MISMATCH &&
               bad_site[0] == 0xe8u, "original-target mismatch modified the callsite")) return 1;

    /* Forced protection failure after the relay is made executable exercises
     * rollback: site remains the original direct call and still executes. */
    unsigned char *fail_site = NULL;
    unsigned char *fail_code = make_call_thunk((void *)original_loader, &fail_site);
    if (!check(fail_code != NULL, "could not create rollback fixture")) return 1;
    hyprlodr_call_hook failed_hook = HYPRLODR_CALL_HOOK_INITIALIZER;
    fault_context fault = {1, fail_site, 0};
    hyprlodr_call_hook_ops ops = {&fault, fault_alloc_near, fault_protect, fault_flush};
    unsigned char *failure_bridge = (unsigned char *)make_far_bridge((void *)near_replacement);
    if (!check(failure_bridge != NULL, "could not create failure bridge")) return 1;
    unsigned char saved[5];
    memcpy(saved, fail_site, sizeof saved);
    hyprlodr_call_hook_status failed = hyprlodr_call_hook_install(
        &failed_hook, fail_site, saved, (void *)original_loader, failure_bridge,
        HYPRLODR_CALL_HOOK_INSTALL_QUIESCENT, &ops);
    if (!check(failed == HYPRLODR_CALL_HOOK_PROTECT_FAILED &&
               bytes_equal(saved, fail_site, sizeof saved) && !failed_hook.installed &&
               page_is_execute_read(fail_site),
               "protection failure did not roll back bytes and page protection")) return 1;

    unsigned char *alloc_fail_site = NULL;
    unsigned char *alloc_fail_code = make_call_thunk((void *)original_loader, &alloc_fail_site);
    if (!check(alloc_fail_code != NULL, "could not create relay-allocation failure fixture")) return 1;
    hyprlodr_call_hook alloc_fail_hook = HYPRLODR_CALL_HOOK_INITIALIZER;
    hyprlodr_call_hook_ops alloc_fail_ops = {NULL, reject_alloc_near, fault_protect, fault_flush};
    unsigned char alloc_saved[5];
    memcpy(alloc_saved, alloc_fail_site, sizeof alloc_saved);
    hyprlodr_call_hook_status alloc_failed = hyprlodr_call_hook_install(
        &alloc_fail_hook, alloc_fail_site, alloc_saved, (void *)original_loader,
        far_bridge, HYPRLODR_CALL_HOOK_INSTALL_QUIESCENT, &alloc_fail_ops);
    if (!check(alloc_failed == HYPRLODR_CALL_HOOK_RELAY_FAILED &&
               bytes_equal(alloc_saved, alloc_fail_site, sizeof alloc_saved),
               "relay allocation failure changed the callsite")) return 1;

    unsigned char *flush_site = NULL;
    unsigned char *flush_code = make_call_thunk((void *)original_loader, &flush_site);
    if (!check(flush_code != NULL, "could not create instruction-cache failure fixture")) return 1;
    hyprlodr_call_hook flush_fail_hook = HYPRLODR_CALL_HOOK_INITIALIZER;
    fault_context flush_fault = {0, flush_site, 1};
    hyprlodr_call_hook_ops flush_ops = {&flush_fault, fault_alloc_near,
                                        fault_protect, fault_flush};
    unsigned char flush_saved[5];
    memcpy(flush_saved, flush_site, sizeof flush_saved);
    hyprlodr_call_hook_status flush_failed = hyprlodr_call_hook_install(
        &flush_fail_hook, flush_site, flush_site, (void *)original_loader,
        far_bridge, HYPRLODR_CALL_HOOK_INSTALL_QUIESCENT, &flush_ops);
    if (!check(flush_failed == HYPRLODR_CALL_HOOK_FLUSH_FAILED &&
               bytes_equal(flush_saved, flush_site, sizeof flush_saved) &&
               !flush_fail_hook.installed && page_is_execute_read(flush_site),
               "instruction-cache failure did not restore bytes and page protection")) return 1;
    g_thunk = (call_thunk_fn)fail_code;
    init_record(record, 1u);
    (void)g_thunk(&context, record, &hand2, 11u);
    if (!check(atomic_load(&g_error_count) == 0,
               "rollback left the synthetic executable unusable")) return 1;

    /* Direct rel32 replacement path: near callback is in the same PE image. */
    unsigned char *direct_site = NULL;
    unsigned char *direct_code = make_call_thunk((void *)original_loader, &direct_site);
    if (!check(direct_code != NULL, "could not create direct-call fixture")) return 1;
    hyprlodr_call_hook direct_hook = HYPRLODR_CALL_HOOK_INITIALIZER;
    unsigned char direct_expected[5];
    memcpy(direct_expected, direct_site, sizeof direct_expected);
    if (!check(hyprlodr_call_hook_install(&direct_hook, direct_site, direct_expected,
              (void *)original_loader, (void *)near_replacement,
              HYPRLODR_CALL_HOOK_INSTALL_QUIESCENT, NULL) == HYPRLODR_CALL_HOOK_OK &&
              !direct_hook.uses_relay, "reachable replacement did not use direct rel32")) return 1;
    g_thunk = (call_thunk_fn)direct_code;
    init_record(record, 1u);
    uintptr_t direct_result = g_thunk(&context, record, &hand2, 12u);
    if (!check(direct_result == ((((uintptr_t)context.marker << 32u) | 12u) ^
                                (uintptr_t)0x55aa55aau),
               "direct rel32 replacement return mismatch")) return 1;
    if (!check(hyprlodr_call_hook_install(&direct_hook, direct_site, direct_expected,
              (void *)original_loader, (void *)original_loader,
              HYPRLODR_CALL_HOOK_INSTALL_QUIESCENT, NULL) ==
              HYPRLODR_CALL_HOOK_ALREADY_DIFFERENT,
              "different repeated install was not rejected")) return 1;

    (void)argc; (void)argv;
    printf("callsite fixture: direct/relay CALL, routing, rollback, concurrency, reentry, and disable passed\n");
    return 0;
}
