#define HYPRLODR_MINIMAL_WIN32 1
#include "../hypr_win32.h"
#include "../route_manifest.h"
#include <stdio.h>
#include <string.h>

static hyprlodr_manifest_result g_result;
static volatile long g_read_error;
static DWORD WINAPI read_route_state(LPVOID unused) {
  (void)unused;
  for (unsigned i = 0; i < 100000u; i++)
    if (g_result.route.name != g_result.name ||
        strcmp(g_result.route.name, "hand2") ||
        g_result.route.replacement_offset != 306794369u ||
        g_result.route.replacement_size != 22056u)
      InterlockedExchange(&g_read_error, 1);
  return 0;
}
int main(int argc, char **argv) {
  if (argc != 6) {
    printf("route_manifest_fixture: expected manifest archive exe "
           "expected_hash mode\n");
    return 2;
  }
  hyprlodr_manifest_inputs in = {argv[3],
                                 argv[2],
                                 argv[1],
                                 argv[4],
                                 "tbl-e91fb3b80004",
                                 "rec-011a0ebc0-000055d4-0001",
                                 295758784u,
                                 21972u,
                                 1u};
  hyprlodr_manifest_status s = hyprlodr_manifest_validate(&in, &g_result);
  if (!strcmp(argv[5], "valid")) {
    if (s != HYPRLODR_MANIFEST_OK) {
      printf("route_manifest_fixture: %s\n", hyprlodr_manifest_status_name(s));
      return 1;
    }
    if (g_result.route.replacement_size != 22056u ||
        g_result.route.replacement_offset == 0 ||
        g_result.route.archive_size != 306816425u ||
        g_result.vertex_count != 239 || g_result.index_count != 1191 ||
        g_result.vertex_stride != 72 || g_result.route.name != g_result.name) {
      printf("route_manifest_fixture: route state mismatch\n");
      return 1;
    }
    if (hyprlodr_manifest_validate(&in, &g_result) != HYPRLODR_MANIFEST_OK ||
        g_result.route.name != g_result.name) {
      printf(
          "route_manifest_fixture: repeated preflight changed route state\n");
      return 1;
    }
    HANDLE threads[8];
    for (unsigned i = 0; i < 8; i++) {
      threads[i] = CreateThread(NULL, 0, read_route_state, NULL, 0, NULL);
      if (!threads[i])
        return 1;
    }
    for (unsigned i = 0; i < 8; i++) {
      WaitForSingleObject(threads[i], INFINITE);
      CloseHandle(threads[i]);
    }
    if (g_read_error) {
      printf("route_manifest_fixture: concurrent immutable read failed\n");
      return 1;
    }
    printf("route_manifest_fixture: real appended archive and concurrent "
           "immutable route state validated\n");
    return 0;
  }
  if (s == HYPRLODR_MANIFEST_OK) {
    printf("route_manifest_fixture: invalid input accepted\n");
    return 1;
  }
  const char *mode = argv[5];
  if (!strncmp(mode, "reject-", 7) &&
      strcmp(hyprlodr_manifest_status_name(s), mode + 7)) {
    printf("route_manifest_fixture: expected %s got %s\n", mode + 7,
           hyprlodr_manifest_status_name(s));
    return 1;
  }
  printf("route_manifest_fixture: rejected %s\n",
         hyprlodr_manifest_status_name(s));
  return 0;
}
