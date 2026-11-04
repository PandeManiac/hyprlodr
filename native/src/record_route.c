#include "hyprlodr/record_route.h"

#include <string.h>

static uint32_t read_u32(const unsigned char *data, size_t offset) {
    uint32_t value;
    memcpy(&value, data + offset, sizeof value);
    return value;
}

static void write_u32(unsigned char *data, size_t offset, uint32_t value) {
    memcpy(data + offset, &value, sizeof value);
}

static int valid_route(const hyprlodr_record_route *route) {
    if (!route || !route->name || !route->name[0] || route->tag != 1u ||
        route->original_size == 0u || route->replacement_size == 0u ||
        route->replacement_size > 0x7fffffffu || route->archive_size == 0u)
        return 0;
    if (route->replacement_offset > route->archive_size ||
        route->replacement_size > route->archive_size - route->replacement_offset)
        return 0;
    return 1;
}

hyprlodr_route_status hyprlodr_record_route_invoke(
    void *record, size_t record_size, const char *requested_name,
    const hyprlodr_record_route *route, hyprlodr_record_loader_fn loader,
    void *loader_context, const void *name_object, uint32_t flags,
    uintptr_t *result) {
    if (!record || record_size < HYPRLODR_GAME_RECORD_SIZE || !requested_name ||
        !loader || !result)
        return HYPRLODR_ROUTE_BAD_ARGUMENT;
    if (!valid_route(route)) return HYPRLODR_ROUTE_BAD_MANIFEST;

    unsigned char *bytes = (unsigned char *)record;
    uint32_t old_offset = read_u32(bytes, HYPRLODR_RECORD_OFFSET_FIELD);
    uint32_t old_size = read_u32(bytes, HYPRLODR_RECORD_SIZE_FIELD);
    uint32_t tag = read_u32(bytes, HYPRLODR_RECORD_TAG_FIELD);
    if (strcmp(requested_name, route->name) != 0 || tag != route->tag ||
        old_offset != route->original_offset || old_size != route->original_size) {
        *result = loader(loader_context, record, name_object, flags);
        return HYPRLODR_ROUTE_PASSTHROUGH;
    }

    write_u32(bytes, HYPRLODR_RECORD_OFFSET_FIELD, route->replacement_offset);
    write_u32(bytes, HYPRLODR_RECORD_SIZE_FIELD, route->replacement_size);
    *result = loader(loader_context, record, name_object, flags);
    /* Do not replace the whole descriptor: the original loader writes its
     * parsed object pointer at +0x48, which the resolver copies into its cache. */
    write_u32(bytes, HYPRLODR_RECORD_OFFSET_FIELD, old_offset);
    write_u32(bytes, HYPRLODR_RECORD_SIZE_FIELD, old_size);
    return HYPRLODR_ROUTE_APPLIED;
}
