/*
 * hyprlodr-host: synthetic game process used for automated loader tests, the
 * reference host for the loader library, and the descriptor oracle used by
 * packaging tools.
 *
 * Modes:
 *   (default)  resolve, initialize, shut down; print a JSON summary
 *   --describe load libraries, validate and print descriptors only; no mod
 *              initialization runs, so packaging tools can cross-check a
 *              manifest against the real ABI without executing mod code
 *
 * --redirect-probe PATH asks the loader which file it would open instead of
 * PATH, exactly as the in-game file hook would, and reports it in the summary.
 * That is what lets the Linux suite assert ABI 4 redirection without Windows.
 *
 * Both modes print one line of JSON on stdout so harnesses assert on the real
 * outcome instead of scraping human text. Events go to the JSON Lines log.
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
            "  --state-root DIR     per-mod state directory root (default: none)\n"
            "  --seed N             randomize in-memory discovery order\n"
            "  --describe           print descriptors and exit without initializing\n"
            "  --redirect-probe P   report which file would open instead of P\n"
            "                       (repeatable, up to 8; ABI 4 file redirection)\n"
            "  -h, --help           show this help\n",
            argv0);
}

static void print_json_string(const char *text) {
    char buffer[1400];
    size_t used = 0;
    buffer[used++] = '"';
    for (const char *cursor = text ? text : ""; *cursor && used + 6 < sizeof buffer; ++cursor) {
        unsigned char c = (unsigned char)*cursor;
        if (c == '"' || c == '\\') {
            buffer[used++] = '\\';
            buffer[used++] = (char)c;
        } else if (c < 0x20) {
            int written = snprintf(buffer + used, sizeof buffer - used, "\\u%04x", (unsigned)c);
            if (written > 0) used += (size_t)written;
        } else {
            buffer[used++] = (char)c;
        }
    }
    buffer[used++] = '"';
    buffer[used] = '\0';
    printf("%s", buffer);
}

static void print_list(const char *const *items, size_t count) {
    printf("[");
    for (size_t i = 0; i < count; ++i) {
        if (i) printf(",");
        print_json_string(items && items[i] ? items[i] : "");
    }
    printf("]");
}

static void print_describe(const hyprlodr_loader *loader, int status) {
    hyprlodr_mod_info mods[256];
    size_t count = hyprlodr_loader_describe_all(loader, mods, sizeof mods / sizeof mods[0]);
    printf("{\"host\":\"hyprlodr\",\"abi\":%u,\"mode\":\"describe\",\"status\":%d,\"mods\":[",
           (unsigned)HYPRLODR_ABI_VERSION, status);
    for (size_t i = 0; i < count; ++i) {
        if (i) printf(",");
        printf("{\"id\":");
        print_json_string(mods[i].id);
        printf(",\"version\":");
        print_json_string(mods[i].version);
        printf(",\"path\":");
        print_json_string(mods[i].library_path);
        printf(",\"dependencies\":");
        print_list(mods[i].dependencies, mods[i].dependency_count);
        printf(",\"conflicts\":");
        print_list(mods[i].conflicts, mods[i].conflict_count);
        printf(",\"flags\":%u,\"disabled\":%s}", mods[i].flags, mods[i].disabled ? "true" : "false");
    }
    printf("],\"errors\":%d}\n", hyprlodr_loader_error_count(loader));
}

#define HYPRLODR_MAX_PROBES 8

static void print_summary(const hyprlodr_loader *loader, int status, const char *const *probes,
                          size_t probe_count, char (*resolved)[HYPRLODR_REDIRECT_PATH_MAX],
                          const int *matched, size_t redirect_count) {
    const char *ids[256];
    size_t count = hyprlodr_loader_order(loader, ids, sizeof ids / sizeof ids[0]);
    printf("{\"host\":\"hyprlodr\",\"abi\":%u,\"status\":%d,\"ok\":%s,\"mods\":%zu,"
           "\"errors\":%d,\"order\":[",
           (unsigned)HYPRLODR_ABI_VERSION, status,
           (status == HYPRLODR_OK && !hyprlodr_loader_had_error(loader)) ? "true" : "false",
           hyprlodr_loader_mod_count(loader), hyprlodr_loader_error_count(loader));
    for (size_t i = 0; i < count; ++i) {
        if (i) printf(",");
        print_json_string(ids[i]);
    }
    printf("],\"redirect_count\":%zu,\"redirects\":[", redirect_count);
    for (size_t i = 0; i < probe_count; ++i) {
        if (i) printf(",");
        printf("{\"requested\":");
        print_json_string(probes[i]);
        printf(",\"redirected\":%s,\"effective\":", matched[i] ? "true" : "false");
        print_json_string(matched[i] ? resolved[i] : probes[i]);
        printf("}");
    }
    printf("],\"last_error\":");
    print_json_string(hyprlodr_loader_last_error(loader));
    printf("}\n");
}

int main(int argc, char **argv) {
    const char *log_path = "hyprlodr-events.jsonl";
    const char *disabled_file = NULL;
    const char *state_root = NULL;
    unsigned seed = 0;
    int describe = 0;
    int ok = 1;
    const char *probes[HYPRLODR_MAX_PROBES];
    size_t probe_count = 0;
    char resolved[HYPRLODR_MAX_PROBES][HYPRLODR_REDIRECT_PATH_MAX];
    int matched[HYPRLODR_MAX_PROBES] = {0};

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
        }        else if (!strcmp(arg, "--disabled-file") && i + 1 < argc) disabled_file = argv[++i];
        else if (!strcmp(arg, "--state-root") && i + 1 < argc) state_root = argv[++i];
        else if (!strcmp(arg, "--seed") && i + 1 < argc) seed = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(arg, "--redirect-probe") && i + 1 < argc) {
            if (probe_count == HYPRLODR_MAX_PROBES) {
                fprintf(stderr, "hyprlodr-host: too many --redirect-probe options\n");
                ok = 0;
            } else {
                probes[probe_count++] = argv[++i];
            }
        }
        else if (!strcmp(arg, "--describe")) describe = 1;
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
    if (status == HYPRLODR_OK && state_root)
        status = hyprlodr_loader_set_state_root(loader, state_root);
    if (status == HYPRLODR_OK && disabled_file)
        status = hyprlodr_loader_disable_file(loader, disabled_file);
    if (status == HYPRLODR_OK) status = hyprlodr_loader_resolve(loader);

    if (describe) {
        print_describe(loader, status);
        int failed = status != HYPRLODR_OK || hyprlodr_loader_had_error(loader);
        hyprlodr_loader_destroy(loader);
        return failed ? 1 : 0;
    }

    if (status == HYPRLODR_OK) status = hyprlodr_loader_init(loader);
    /* Resolve the probes before shutdown: a mod that signs out must stop
     * redirecting, which the suite asserts separately. */
    for (size_t i = 0; i < probe_count; ++i)
        matched[i] = hyprlodr_loader_redirect_path(loader, probes[i], resolved[i],
                                                   sizeof resolved[i]);
    /* Read the count before shutdown: a mod signing out stops redirecting. */
    size_t redirect_count = hyprlodr_loader_redirect_count(loader);
    hyprlodr_loader_shutdown(loader);
    print_summary(loader, status, probes, probe_count, resolved, matched, redirect_count);

    int failed = status != HYPRLODR_OK || hyprlodr_loader_had_error(loader);
    hyprlodr_loader_destroy(loader);
    return failed ? 1 : 0;
}
