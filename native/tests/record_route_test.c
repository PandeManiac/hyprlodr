#include "hyprlodr/record_route.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#endif

typedef struct {
    uint32_t seen_offset;
    uint32_t seen_size;
    uint32_t seen_tag;
    const void *seen_name_object;
    unsigned calls;
} loader_probe;

static void put_u32(unsigned char *record, size_t offset, uint32_t value) {
    memcpy(record + offset, &value, sizeof value);
}

static uint32_t get_u32(const unsigned char *record, size_t offset) {
    uint32_t value;
    memcpy(&value, record + offset, sizeof value);
    return value;
}

static uintptr_t fake_loader(void *context, void *record, const void *name_object,
                             uint32_t flags) {
    loader_probe *probe = (loader_probe *)context;
    unsigned char *bytes = (unsigned char *)record;
    (void)flags;
    probe->seen_offset = get_u32(bytes, HYPRLODR_RECORD_OFFSET_FIELD);
    probe->seen_size = get_u32(bytes, HYPRLODR_RECORD_SIZE_FIELD);
    probe->seen_tag = get_u32(bytes, HYPRLODR_RECORD_TAG_FIELD);
    probe->seen_name_object = name_object;
    ++probe->calls;
    /* Emulate the real tag-1 loader caching its parsed object at +0x48. */
    uintptr_t object = (uintptr_t)0x12345678u;
    memcpy(bytes + HYPRLODR_RECORD_OBJECT_FIELD, &object, sizeof object);
    return (uintptr_t)0xabcdefu;
}

static hyprlodr_record_route route_fixture(void) {
    hyprlodr_record_route route = {
        "hand2", 295758784u, 21972u, 1u, 306794369u, 22056u, 306816425u
    };
    return route;
}

static void init_record(unsigned char record[HYPRLODR_GAME_RECORD_SIZE],
                       uint32_t offset, uint32_t size, uint32_t tag) {
    memset(record, 0xa5, HYPRLODR_GAME_RECORD_SIZE);
    put_u32(record, HYPRLODR_RECORD_OFFSET_FIELD, offset);
    put_u32(record, HYPRLODR_RECORD_SIZE_FIELD, size);
    put_u32(record, HYPRLODR_RECORD_TAG_FIELD, tag);
}

static void test_target_uses_replacement_and_restores_metadata(void) {
    unsigned char record[HYPRLODR_GAME_RECORD_SIZE];
    loader_probe probe = {0};
    hyprlodr_record_route route = route_fixture();
    uintptr_t result = 0;
    const char name_object[] = "synthetic MSVC string object";
    init_record(record, route.original_offset, route.original_size, 1u);
    assert(hyprlodr_record_route_invoke(record, sizeof record, "hand2", &route,
        fake_loader, &probe, name_object, 0u, &result) == HYPRLODR_ROUTE_APPLIED);
    assert(result == (uintptr_t)0xabcdefu);
    assert(probe.calls == 1u);
    assert(probe.seen_offset == route.replacement_offset);
    assert(probe.seen_size == route.replacement_size);
    assert(probe.seen_tag == 1u && probe.seen_name_object == name_object);
    assert(get_u32(record, 0u) == route.original_offset);
    assert(get_u32(record, 4u) == route.original_size);
    uintptr_t object = 0;
    memcpy(&object, record + HYPRLODR_RECORD_OBJECT_FIELD, sizeof object);
    assert(object == (uintptr_t)0x12345678u);
}

static void test_tag_two_and_unrelated_requests_pass_through(void) {
    unsigned char record[HYPRLODR_GAME_RECORD_SIZE];
    loader_probe probe = {0};
    hyprlodr_record_route route = route_fixture();
    uintptr_t result = 0;
    init_record(record, route.original_offset, route.original_size, 2u);
    assert(hyprlodr_record_route_invoke(record, sizeof record, "hand2", &route,
        fake_loader, &probe, NULL, 0u, &result) == HYPRLODR_ROUTE_PASSTHROUGH);
    assert(probe.seen_offset == route.original_offset && probe.seen_size == route.original_size);
    assert(probe.seen_tag == 2u);
    init_record(record, route.original_offset, route.original_size, 1u);
    assert(hyprlodr_record_route_invoke(record, sizeof record, "hand2left", &route,
        fake_loader, &probe, NULL, 0u, &result) == HYPRLODR_ROUTE_PASSTHROUGH);
    assert(probe.seen_offset == route.original_offset && probe.seen_size == route.original_size);
}

static void test_metadata_mismatch_fails_open_to_original_record(void) {
    unsigned char record[HYPRLODR_GAME_RECORD_SIZE];
    loader_probe probe = {0};
    hyprlodr_record_route route = route_fixture();
    uintptr_t result = 0;
    init_record(record, route.original_offset + 1u, route.original_size, 1u);
    assert(hyprlodr_record_route_invoke(record, sizeof record, "hand2", &route,
        fake_loader, &probe, NULL, 0u, &result) == HYPRLODR_ROUTE_PASSTHROUGH);
    assert(probe.seen_offset == route.original_offset + 1u);
    assert(probe.seen_size == route.original_size);
}

static void test_bad_route_is_rejected_without_calling_loader(void) {
    unsigned char record[HYPRLODR_GAME_RECORD_SIZE];
    loader_probe probe = {0};
    hyprlodr_record_route route = route_fixture();
    uintptr_t result = 0;
    init_record(record, route.original_offset, route.original_size, 1u);
    route.replacement_size = route.archive_size;
    assert(hyprlodr_record_route_invoke(record, sizeof record, "hand2", &route,
        fake_loader, &probe, NULL, 0u, &result) == HYPRLODR_ROUTE_BAD_MANIFEST);
    assert(probe.calls == 0u);
    assert(get_u32(record, 0u) == route.original_offset);
}

typedef struct {
    unsigned char record[HYPRLODR_GAME_RECORD_SIZE];
    hyprlodr_record_route route;
    loader_probe probe;
    unsigned iterations;
} concurrent_case;

static void *run_concurrent_case(void *opaque) {
    concurrent_case *test = (concurrent_case *)opaque;
    uintptr_t result = 0;
    for (unsigned iteration = 0; iteration < test->iterations; ++iteration) {
        test->probe.calls = 0;
        assert(hyprlodr_record_route_invoke(test->record, sizeof test->record,
            "hand2", &test->route, fake_loader, &test->probe, NULL, 0u,
            &result) == HYPRLODR_ROUTE_APPLIED);
        assert(test->probe.seen_offset == test->route.replacement_offset);
        assert(test->probe.seen_size == test->route.replacement_size);
        assert(get_u32(test->record, 0u) == test->route.original_offset);
        assert(get_u32(test->record, 4u) == test->route.original_size);
    }
    return NULL;
}

static void test_independent_concurrent_requests(void) {
#if defined(__unix__) || defined(__APPLE__)
    enum { THREADS = 8 };
    pthread_t threads[THREADS];
    concurrent_case cases[THREADS];
    for (unsigned index = 0; index < THREADS; ++index) {
        cases[index].route = route_fixture();
        cases[index].iterations = 1000u;
        memset(&cases[index].probe, 0, sizeof cases[index].probe);
        init_record(cases[index].record, cases[index].route.original_offset,
                    cases[index].route.original_size, 1u);
        assert(pthread_create(&threads[index], NULL, run_concurrent_case,
                              &cases[index]) == 0);
    }
    for (unsigned index = 0; index < THREADS; ++index)
        assert(pthread_join(threads[index], NULL) == 0);
#endif
}

typedef struct {
    loader_probe probe;
    const hyprlodr_record_route *route;
    unsigned depth;
    hyprlodr_route_status nested_status;
} reentrant_probe;

static uintptr_t reentrant_loader(void *context, void *record,
                                  const void *name_object, uint32_t flags) {
    reentrant_probe *probe = (reentrant_probe *)context;
    if (probe->depth == 0u) {
        uintptr_t nested_result = 0;
        ++probe->depth;
        probe->nested_status = hyprlodr_record_route_invoke(record,
            HYPRLODR_GAME_RECORD_SIZE, "hand2", probe->route,
            reentrant_loader, context, name_object, flags, &nested_result);
        --probe->depth;
    }
    return fake_loader(&probe->probe, record, name_object, flags);
}

static void test_reentrant_call_uses_consistent_pair(void) {
    unsigned char record[HYPRLODR_GAME_RECORD_SIZE];
    hyprlodr_record_route route = route_fixture();
    reentrant_probe probe;
    uintptr_t result = 0;
    memset(&probe, 0, sizeof probe);
    probe.route = &route;
    init_record(record, route.original_offset, route.original_size, 1u);
    assert(hyprlodr_record_route_invoke(record, sizeof record, "hand2", &route,
        reentrant_loader, &probe, NULL, 0u, &result) == HYPRLODR_ROUTE_APPLIED);
    assert(probe.nested_status == HYPRLODR_ROUTE_PASSTHROUGH);
    assert(probe.probe.calls == 2u);
    assert(probe.probe.seen_offset == route.replacement_offset);
    assert(probe.probe.seen_size == route.replacement_size);
    assert(get_u32(record, 0u) == route.original_offset);
    assert(get_u32(record, 4u) == route.original_size);
}

int main(void) {
    test_target_uses_replacement_and_restores_metadata();
    test_tag_two_and_unrelated_requests_pass_through();
    test_metadata_mismatch_fails_open_to_original_record();
    test_bad_route_is_rejected_without_calling_loader();
    test_independent_concurrent_requests();
    test_reentrant_call_uses_consistent_pair();
    puts("record_route_test: per-request metadata routing passed");
    return 0;
}
