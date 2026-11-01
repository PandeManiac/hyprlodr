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
 * --image-file PATH maps that file as the "game image" for ABI 5 tests: mods can
 * then read it through host_api.image_read(), exactly as they would read the game
 * in the proxy. The file is not executable and build_matches is set to true.
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
            "  --image-file PATH    offer PATH as the game image to mods (ABI 5)\n"
            "  --image-out PATH     write the image back after mods shut down (ABI 6)\n"
            "  --symbol NAME@RVA:LEN publish a named, hash-verified location (ABI 7)\n"
            "  --symbol-stale ...   same, but the bytes are altered so lookup must fail\n"
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

/* --- ABI 5 image fixture ----------------------------------------------------- */
static unsigned char *image_bytes;
static size_t image_size;

/* Read a file into memory so mods can read it through host_api.image_read().
 * The Windows host links HYPRLODR's minimal CRT, which has no seeking, so the
 * option is Linux-only: the in-game image comes from the proxy instead. */
#ifndef HYPRLODR_MINIMAL_WIN32
static int load_image_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    long length = ftell(file);
    if (length <= 0 || length > (16L * 1024L * 1024L)) {
        fclose(file);
        return -1;
    }
    rewind(file);
    unsigned char *buffer = (unsigned char *)malloc((size_t)length);
    if (!buffer) {
        fclose(file);
        return -1;
    }
    if (fread(buffer, 1, (size_t)length, file) != (size_t)length) {
        free(buffer);
        fclose(file);
        return -1;
    }
    fclose(file);
    image_bytes = buffer;
    image_size = (size_t)length;
    return 0;
}
#else
static int load_image_file(const char *path) {
    (void)path;
    return -1; /* no --image-file support in the minimal-CRT host */
}
#endif

/* The host owns the image buffer, so it performs patches itself; the loader only
 * validates them, journals the original bytes and asks for restores. */
static int image_write_bytes(void *context, void *destination, const void *source, size_t length) {
    (void)context;
    if (!destination || !source || length == 0) return -1;
    memcpy(destination, source, length);
    return 0;
}

#define HYPRLODR_MAX_TEST_SYMBOLS 16

typedef struct {
    char name[HYPRLODR_SYMBOL_NAME_MAX];
    unsigned long rva;
    unsigned long length;
    int stale;
} test_symbol_spec;

static test_symbol_spec test_symbol_specs[HYPRLODR_MAX_TEST_SYMBOLS];
static size_t test_symbol_count;
static hyprlodr_anchor test_symbol_table[HYPRLODR_MAX_TEST_SYMBOLS];

/* Parse NAME@RVA:LEN without strtoull, which the minimal CRT does not have. */
static int parse_symbol_spec(const char *text, test_symbol_spec *out, int stale) {
    size_t used = 0;
    while (text[used] && text[used] != '@') {
        if (used + 1u >= sizeof out->name) return -1;
        out->name[used] = text[used];
        ++used;
    }
    if (!used || text[used] != '@') return -1;
    out->name[used] = '\0';

    const char *cursor = text + used + 1u;
    char *end = NULL;
    out->rva = strtoul(cursor, &end, 0);
    if (!end || *end != ':') return -1;
    out->length = strtoul(end + 1, NULL, 0);
    if (!out->length) return -1;
    out->stale = stale;
    return 0;
}

/* Hash the bytes the fixture image really contains, so the symbol table describes
 * the image under test instead of a hardcoded truth. A stale entry is hashed
 * first and then altered, which is exactly the "patched build" case. */
static int build_symbol_table(void) {
    if (test_symbol_count == 0) return 0;
    if (!image_bytes || image_size == 0) return -1;
    for (size_t index = 0; index < test_symbol_count; ++index) {
        const test_symbol_spec *spec = &test_symbol_specs[index];
        test_symbol_table[index].name = spec->name;
        test_symbol_table[index].rva = (uint32_t)spec->rva;
        test_symbol_table[index].length = (uint32_t)spec->length;
        /* An entry outside this image is kept, with a hash no lookup can match:
         * that is exactly what a table for a different build looks like, and the
         * loader must refuse it rather than abort the run. */
        if (spec->rva > (unsigned long)image_size ||
            spec->length > (unsigned long)image_size - spec->rva) {
            test_symbol_table[index].fnv1a64 = 0;
            continue;
        }
        test_symbol_table[index].fnv1a64 =
            hyprlodr_fnv1a64(image_bytes + spec->rva, (size_t)spec->length);
        if (spec->stale) image_bytes[spec->rva] ^= 0x01u;
    }
    return 0;
}

/* Dump the image after the loader has restored every patch, so a test can
 * compare the result with the bytes it supplied. Like --image-file this needs
 * stdio seeking/writing the minimal CRT does not have, so it is Linux-only. */
#ifndef HYPRLODR_MINIMAL_WIN32
static int dump_image_file(const char *path) {
    if (!image_bytes || image_size == 0) return -1;
    FILE *file = fopen(path, "wb");
    if (!file) return -1;
    size_t written = fwrite(image_bytes, 1, image_size, file);
    fclose(file);
    return written == image_size ? 0 : -1;
}
#else
static int dump_image_file(const char *path) {
    (void)path;
    return 0; /* nothing to dump without --image-file */
}
#endif

int main(int argc, char **argv) {
    const char *log_path = "hyprlodr-events.jsonl";
    const char *disabled_file = NULL;
    const char *state_root = NULL;
    const char *image_file = NULL;
    const char *image_out = NULL;
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
        else if (!strcmp(arg, "--image-file") && i + 1 < argc) image_file = argv[++i];
        else if (!strcmp(arg, "--image-out") && i + 1 < argc) image_out = argv[++i];
        else if ((!strcmp(arg, "--symbol") || !strcmp(arg, "--symbol-stale")) && i + 1 < argc) {
            int stale = !strcmp(arg, "--symbol-stale");
            const char *spec = argv[++i];
            if (test_symbol_count == HYPRLODR_MAX_TEST_SYMBOLS) {
                fprintf(stderr, "hyprlodr-host: too many symbols\n");
                ok = 0;
            } else if (parse_symbol_spec(spec, &test_symbol_specs[test_symbol_count], stale) != 0) {
                fprintf(stderr, "hyprlodr-host: bad symbol spec %s\n", spec);
                ok = 0;
            } else {
                ++test_symbol_count;
            }
        }
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

    if (ok && image_file && load_image_file(image_file) != 0) {
        fprintf(stderr, "hyprlodr-host: cannot read --image-file %s\n", image_file);
        ok = 0;
    }
    if (ok && test_symbol_count && build_symbol_table() != 0) {
        fprintf(stderr, "hyprlodr-host: symbol range is outside the image\n");
        ok = 0;
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
    if (status == HYPRLODR_OK && image_file) {
        hyprlodr_image image;
        image.base = image_bytes;
        image.size = image_size;
        image.build_matches = 1; /* the test harness declares this the image */
        image.write_context = NULL;
        image.write = image_write_bytes;
        status = hyprlodr_loader_set_image(loader, &image);
    }
    if (status == HYPRLODR_OK && test_symbol_count)
        status = hyprlodr_loader_set_symbols(loader, test_symbol_table, test_symbol_count);
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
    /* Destruction restores every patch, so this dump proves restoration. */
    if (image_out && dump_image_file(image_out) != 0) {
        fprintf(stderr, "hyprlodr-host: cannot write --image-out %s\n", image_out);
        failed = 1;
    }
    return failed ? 1 : 0;
}
