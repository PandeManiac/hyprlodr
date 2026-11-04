/* Narrow, allocation-free request routing for the verified v3 tag-1 loader.
 * This module does not install a code hook. The caller must invoke it only
 * from the verified loader callsite after validating the executable, archive,
 * and immutable manifest. */
#ifndef HYPRLODR_RECORD_ROUTE_H
#define HYPRLODR_RECORD_ROUTE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HYPRLODR_GAME_RECORD_SIZE 0x58u
#define HYPRLODR_RECORD_OFFSET_FIELD 0u
#define HYPRLODR_RECORD_SIZE_FIELD 4u
#define HYPRLODR_RECORD_TAG_FIELD 0x40u
#define HYPRLODR_RECORD_OBJECT_FIELD 0x48u

typedef struct {
    const char *name;
    uint32_t original_offset;
    uint32_t original_size;
    uint32_t tag;
    uint32_t replacement_offset;
    uint32_t replacement_size;
    uint32_t archive_size;
} hyprlodr_record_route;

typedef uintptr_t (*hyprlodr_record_loader_fn)(void *context, void *record,
                                               const void *name_object, uint32_t flags);

typedef enum {
    HYPRLODR_ROUTE_BAD_ARGUMENT = -1,
    HYPRLODR_ROUTE_BAD_MANIFEST = -2,
    HYPRLODR_ROUTE_PASSTHROUGH = 0,
    HYPRLODR_ROUTE_APPLIED = 1
} hyprlodr_route_status;

/* Invoke the original loader with one coherent replacement offset/size pair.
 * The descriptor is the resolver's per-request 0x58-byte stack copy. The pair
 * is restored even when the callback fails, while any parsed-object cache field
 * written by the loader is retained for the caller's ordinary cache copy. */
hyprlodr_route_status hyprlodr_record_route_invoke(
    void *record, size_t record_size, const char *requested_name,
    const hyprlodr_record_route *route, hyprlodr_record_loader_fn loader,
    void *loader_context, const void *name_object, uint32_t flags,
    uintptr_t *result);

#ifdef __cplusplus
}
#endif
#endif
