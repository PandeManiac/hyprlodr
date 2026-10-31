/*
 * hyprlodr-host: synthetic game process used for automated loader tests and as
 * the reference host for the loader library.
 *
 * It prints a single-line JSON summary on stdout so test harnesses can assert
 * on the real outcome instead of scraping human text. Events are also written
 * as JSON Lines to the configured log.
 */
#include "loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [options] <mod-directory>...\n"
            "  --dir DIR            add a mod directory (repeatable)\n"
            "  --log PATH           event log destination (JSON Lines)\n"
            "  --disable ID         mark a mod installed-but-disabled (repeatable)\n"
            "  --disabled-file PATH mod ids to disable, one per line\n"
            "  --seed N             randomize in-memory discovery order\n"
            "  -h, --help           show this help\n",
            argv0);
}

static void print_summary(const hyprlodr_loader *loader, int status) {
    const char *ids[256];
    size_t count = hyprlodr_loader_order(loader, ids, sizeof ids / sizeof ids[0]);
    printf("{\"host\":\"hyprlodr\",\"abi\":%u,\"status\":%d,\"ok\":%s,\"mods\":%zu,"
           "\"errors\":%d,\"order\":[",
           (unsigned)HYPRLODR_ABI_VERSION, status,
           (status == HYPRLODR_OK && !hyprlodr_loader_had_error(loader)) ? "true" : "false",
           hyprlodr_loader_mod_count(loader), hyprlodr_loader_error_count(loader));
    for (size_t i = 0; i < count; ++i) printf("%s\"%s\"", i ? "," : "", ids[i]);
    printf("],\"last_error\":\"%s\"}\n", hyprlodr_loader_last_error(loader));
}

int main(int argc, char **argv) {
    const char *log_path = "hyprlodr-events.jsonl";
    const char *disabled_file = NULL;
    unsigned seed = 0;
    int ok = 1;

    hyprlodr_loader *loader = hyprlodr_loader_create();
    if (!loader) {
        fprintf(stderr, "hyprlodr-host: out of memory\n");
        return 2;
    }

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--log") && i + 1 < argc) log_path = argv[++i];
        else if (!strcmp(arg, "--dir") && i + 1 < argc) {
            if (hyprlodr_loader_add_dir(loader, argv[++i]) != HYPRLODR_OK) ok = 0;
        } else if (!strcmp(arg, "--disable") && i + 1 < argc) {
            if (hyprlodr_loader_disable(loader, argv[++i]) != HYPRLODR_OK) ok = 0;
        } else if (!strcmp(arg, "--disabled-file") && i + 1 < argc) disabled_file = argv[++i];
        else if (!strcmp(arg, "--seed") && i + 1 < argc) seed = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            usage(argv[0]);
            hyprlodr_loader_destroy(loader);
            return 0;
        } else if (arg[0] == '-') {
            fprintf(stderr, "hyprlodr-host: unknown option %s\n", arg);
            usage(argv[0]);
            hyprlodr_loader_destroy(loader);
            return 2;
        } else if (hyprlodr_loader_add_dir(loader, arg) != HYPRLODR_OK) {
            ok = 0;
        }
    }

    if (!ok) {
        fprintf(stderr, "hyprlodr-host: invalid arguments\n");
        hyprlodr_loader_destroy(loader);
        return 2;
    }

    hyprlodr_loader_set_seed(loader, seed);
    int status = hyprlodr_loader_set_log(loader, log_path);
    if (status == HYPRLODR_OK && disabled_file)
        status = hyprlodr_loader_disable_file(loader, disabled_file);
    if (status == HYPRLODR_OK) status = hyprlodr_loader_resolve(loader);
    if (status == HYPRLODR_OK) status = hyprlodr_loader_init(loader);

    hyprlodr_loader_shutdown(loader);
    print_summary(loader, status);

    int failed = status != HYPRLODR_OK || hyprlodr_loader_had_error(loader);
    hyprlodr_loader_destroy(loader);
    return failed ? 1 : 0;
}
