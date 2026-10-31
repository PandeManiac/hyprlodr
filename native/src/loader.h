#ifndef HYPRLODR_LOADER_H
#define HYPRLODR_LOADER_H

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

/* Introspection for hosts and tests. */
size_t hyprlodr_loader_order(const hyprlodr_loader *loader, const char **ids, size_t capacity);
size_t hyprlodr_loader_mod_count(const hyprlodr_loader *loader);
int hyprlodr_loader_error_count(const hyprlodr_loader *loader);
const char *hyprlodr_loader_last_error(const hyprlodr_loader *loader);
int hyprlodr_loader_had_error(const hyprlodr_loader *loader);
const char *hyprlodr_loader_strerror(int status);

#endif
