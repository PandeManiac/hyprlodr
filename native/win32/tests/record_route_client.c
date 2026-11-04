#include "hyprlodr/record_route.h"

#include <stdio.h>
#include <string.h>

typedef struct { uint32_t offset, size, tag; unsigned calls; } probe;

static uint32_t get32(const unsigned char *record, size_t offset) {
    uint32_t value;
    memcpy(&value, record + offset, sizeof value);
    return value;
}

static uintptr_t read_record(void *context, void *record, const void *name,
                             uint32_t flags) {
    probe *seen = (probe *)context;
    unsigned char *bytes = (unsigned char *)record;
    (void)name;
    (void)flags;
    seen->offset = get32(bytes, HYPRLODR_RECORD_OFFSET_FIELD);
    seen->size = get32(bytes, HYPRLODR_RECORD_SIZE_FIELD);
    seen->tag = get32(bytes, HYPRLODR_RECORD_TAG_FIELD);
    ++seen->calls;
    return 1u;
}

static void put32(unsigned char *record, size_t offset, uint32_t value) {
    memcpy(record + offset, &value, sizeof value);
}

int main(void) {
    unsigned char record[HYPRLODR_GAME_RECORD_SIZE];
    hyprlodr_record_route route = {
        "hand2", 295758784u, 21972u, 1u, 306794369u, 22056u, 306816425u
    };
    probe seen = {0};
    uintptr_t result = 0;
    memset(record, 0, sizeof record);
    put32(record, 0u, route.original_offset);
    put32(record, 4u, route.original_size);
    put32(record, HYPRLODR_RECORD_TAG_FIELD, 1u);
    if (hyprlodr_record_route_invoke(record, sizeof record, "hand2", &route,
            read_record, &seen, NULL, 0u, &result) != HYPRLODR_ROUTE_APPLIED ||
        result != 1u || seen.calls != 1u || seen.offset != route.replacement_offset ||
        seen.size != route.replacement_size || seen.tag != 1u ||
        get32(record, 0u) != route.original_offset ||
        get32(record, 4u) != route.original_size) {
        printf("record_route_client: FAILED\n");
        return 1;
    }
    put32(record, HYPRLODR_RECORD_TAG_FIELD, 2u);
    seen.calls = 0u;
    if (hyprlodr_record_route_invoke(record, sizeof record, "hand2", &route,
            read_record, &seen, NULL, 0u, &result) != HYPRLODR_ROUTE_PASSTHROUGH ||
        seen.calls != 1u || seen.offset != route.original_offset || seen.size != route.original_size) {
        printf("record_route_client: tag-2 rejection FAILED\n");
        return 2;
    }
    printf("record_route_client: replacement and tag-2 checks passed\n");
    return 0;
}
