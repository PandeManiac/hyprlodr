#ifndef HYPRLODR_ROUTE_MANIFEST_H
#define HYPRLODR_ROUTE_MANIFEST_H

#include "../include/hyprlodr/record_route.h"

typedef enum {
  HYPRLODR_MANIFEST_OK = 0,
  HYPRLODR_MANIFEST_IO = -1,
  HYPRLODR_MANIFEST_MALFORMED = -2,
  HYPRLODR_MANIFEST_UNSUPPORTED = -3,
  HYPRLODR_MANIFEST_IDENTITY = -4,
  HYPRLODR_MANIFEST_ARCHIVE = -5,
  HYPRLODR_MANIFEST_RESOURCE = -6,
  HYPRLODR_MANIFEST_PAYLOAD = -7,
  HYPRLODR_MANIFEST_LIMIT = -8
} hyprlodr_manifest_status;

typedef struct {
  const char *executable_path;
  const char *archive_path;
  const char *manifest_path;
  const char *supported_executable_sha256;
  const char *resolver_table_id;
  const char *record_id;
  uint32_t original_offset;
  uint32_t original_size;
  uint32_t type_tag;
} hyprlodr_manifest_inputs;

typedef struct {
  hyprlodr_record_route route;
  char name[16];
  char executable_sha256[65];
  char source_sha256[65];
  char output_sha256[65];
  char replacement_sha256[65];
  uint32_t vertex_count;
  uint32_t index_count;
  uint32_t vertex_stride;
} hyprlodr_manifest_result;

/* Strictly validates schema-1 append-only manifests, file identities and the
 * appended ciphertext. The selected archive must have the source archive as
 * its exact byte prefix; the resolver identity comes from pinned native data.
 */
hyprlodr_manifest_status
hyprlodr_manifest_validate(const hyprlodr_manifest_inputs *inputs,
                           hyprlodr_manifest_result *result);

const char *hyprlodr_manifest_status_name(hyprlodr_manifest_status status);

#endif
