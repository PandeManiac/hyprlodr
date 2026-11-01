#ifndef HYPRLODR_LOADER_H
#define HYPRLODR_LOADER_H

#include "hyprlodr/anchors.h"
#include "hyprlodr/plugin.h"

typedef struct hyprlodr_loader hyprlodr_loader;

/* Status codes returned by the loader. */
#define HYPRLODR_OK 0
#define HYPRLODR_ERR_ARG (-1)
#define HYPRLODR_ERR_IO (-2)
#define HYPRLODR_ERR_DEPENDENCY (-3)
#define HYPRLODR_ERR_CYCLE (-4)
#define HYPRLODR_ERR_CONFLICT (-5)
#define HYPRLODR_ERR_INIT (-6)
#define HYPRLODR_ERR_LIMIT (-7)

hyprlodr_loader *hyprlodr_loader_create(void);

/* Append a directory to scan for mod libraries. May be called repeatedly. */
int hyprlodr_loader_add_dir(hyprlodr_loader *loader, const char *directory);

/* Mark a mod id as installed-but-disabled. Dependents of a disabled mod fail. */
int hyprlodr_loader_disable(hyprlodr_loader *loader, const char *mod_id);

/* Read mod ids to disable, one per line; '#' starts a comment, blank lines are
 * ignored. A missing file is not an error. */
int hyprlodr_loader_disable_file(hyprlodr_loader *loader, const char *path);

/* Event log destination (JSON Lines). Must be set before resolve. */
int hyprlodr_loader_set_log(hyprlodr_loader *loader, const char *path);

/* The running game image a host can offer to mods (ABI 5). Reads are refused
 * unless build_matches is non-zero, so a mod can never act on offsets taken from
 * a different executable. Passing NULL clears the image. */
typedef struct {
    const void *base;
    size_t size;
    int build_matches;
    /* ABI 6: when `write` is non-NULL the host accepts mod patches and this
     * callback performs them, so a host that must change page protection (the
     * Windows proxy) stays in control. A host that leaves it NULL keeps the
     * image read-only and every write is refused. */
    void *write_context;
    int (*write)(void *write_context, void *destination, const void *source, size_t length);
} hyprlodr_image;

int hyprlodr_loader_set_image(hyprlodr_loader *loader, const hyprlodr_image *image);

/* Named game locations published to mods (ABI 7). Each entry carries the FNV-1a
 * 64 hash of the bytes expected at that RVA; symbol_lookup() re-verifies the hash
 * against the live image, so an entry can never resolve to a patched build. The
 * table is copied, so the caller's storage need not outlive this call. */
#define HYPRLODR_MAX_SYMBOLS 128
#define HYPRLODR_SYMBOL_NAME_MAX 64

int hyprlodr_loader_set_symbols(hyprlodr_loader *loader, const hyprlodr_anchor *symbols,
                                size_t count);

/* Root for per-mod persistent directories. Each initialized mod gets
 * <root>/<mod-id>, created before its initialize() runs and exposed through
 * host_api.state_dir(). Unset by default, in which case state_dir() is NULL. */
int hyprlodr_loader_set_state_root(hyprlodr_loader *loader, const char *path);

/* Non-zero seed randomizes the in-memory discovery order. Resolution order
 * must not depend on it; the test suite uses this to prove determinism. */
void hyprlodr_loader_set_seed(hyprlodr_loader *loader, unsigned seed);

/* Load libraries, validate descriptors, then compute the initialization order.
 * No mod code runs during this phase. */
int hyprlodr_loader_resolve(hyprlodr_loader *loader);

/* Initialize mods in resolved order, rolling back on the first failure. */
int hyprlodr_loader_init(hyprlodr_loader *loader);

/* Convenience: create-time setup for the common single-directory case.
 * Equivalent to add_dir + set_log + resolve + init. */
int hyprlodr_loader_run(hyprlodr_loader *loader, const char *plugin_directory,
                        const char *event_log_path);

/* Shut down initialized mods in reverse order. Idempotent. */
void hyprlodr_loader_shutdown(hyprlodr_loader *loader);

/* Shutdown (if needed), unload libraries and release all memory. */
void hyprlodr_loader_destroy(hyprlodr_loader *loader);

/* Static metadata for one discovered mod, without running any mod code. */
typedef struct {
    const char *id;
    const char *version;
    const char *library_path;
    const char *const *dependencies;
    size_t dependency_count;
    const char *const *conflicts;
    size_t conflict_count;
    uint32_t flags;
    int disabled;
} hyprlodr_mod_info;

/* Fill `out` with descriptors in mod-id order. Returns the number written. */
size_t hyprlodr_loader_describe_all(const hyprlodr_loader *loader, hyprlodr_mod_info *out,
                                    size_t capacity);

/* Introspection for hosts and tests. */
size_t hyprlodr_loader_order(const hyprlodr_loader *loader, const char **ids, size_t capacity);
size_t hyprlodr_loader_mod_count(const hyprlodr_loader *loader);
int hyprlodr_loader_error_count(const hyprlodr_loader *loader);
const char *hyprlodr_loader_last_error(const hyprlodr_loader *loader);
int hyprlodr_loader_had_error(const hyprlodr_loader *loader);
const char *hyprlodr_loader_strerror(int status);

/* --- file redirection (ABI 4) ----------------------------------------------
 * Mods register suffix -> replacement pairs through host_api.file_redirect().
 * The loader publishes the registry when every mod has initialized; a host that
 * controls file opening (the Windows proxy patches the game's own CreateFileA
 * import slot) asks which path to open instead.
 *
 * Returns 1 and writes the replacement path into `out` when `requested` matches
 * a published suffix, 0 otherwise. A path that does not fit is never truncated:
 * that returns 0 rather than opening the wrong file. */
#define HYPRLODR_REDIRECT_PATH_MAX 1024

int hyprlodr_loader_redirect_path(const hyprlodr_loader *loader, const char *requested,
                                  char *out, size_t capacity);

/* Number of published redirects (0 until initialization succeeded). */
size_t hyprlodr_loader_redirect_count(const hyprlodr_loader *loader);

#endif
