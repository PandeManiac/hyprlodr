#include "loader.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <plugin-directory> <event-log.jsonl>\n", argv[0]); return 2; }
    hyprlodr_loader *loader = hyprlodr_loader_create();
    if (!loader) return 2;
    int result = hyprlodr_loader_run(loader, argv[1], argv[2]);
    hyprlodr_loader_shutdown(loader);
    int errors = hyprlodr_loader_had_error(loader);
    hyprlodr_loader_destroy(loader);
    return result || errors ? 1 : 0;
}
