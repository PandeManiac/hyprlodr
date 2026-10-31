/*
 * HYPRLODR native loader: discovery, ABI validation, deterministic dependency
 * resolution, lifecycle management with rollback, and structured event logging.
 *
 * Phases are deliberately separate:
 *   resolve() -> load libraries + validate descriptors + order mods (no mod code)
 *   init()    -> run initialize() in resolved order, roll back on failure
 *   shutdown()-> run shutdown() in reverse order
 *   destroy() -> unload libraries, release memory
 *
 * Libraries are only unloaded in destroy(), after every callback has returned,
 * so no mod code can be executing from unloaded memory.
 */
#define _POSIX_C_SOURCE 200809L
#include "loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifdef HYPRLODR_MINIMAL_WIN32
#    include "hypr_win32.h"
#  else
#    include <windows.h>
#  endif
#  define PATH_SEP '\\'
#  define LIB_EXT ".dll"
#else
#  include <dirent.h>
#  include <dlfcn.h>
#  include <errno.h>
#  include <sys/stat.h>
#  define PATH_SEP '/'
#  ifdef __APPLE__
#    define LIB_EXT ".dylib"
#  else
#    define LIB_EXT ".so"
#  endif
#endif

#define HYPRLODR_MAX_PLUGINS 128
#define HYPRLODR_MAX_PAIRS 256
#define HYPRLODR_MAX_CAPABILITIES 128
#define HYPRLODR_MAX_UNDO 512
#define HYPRLODR_MAX_DIRS 8
#define HYPRLODR_MAX_DISABLED 128
#define HYPRLODR_ID_MAX 128
#define HYPRLODR_PATH_MAX 1024

typedef struct {
    char *key;
    char *value;
    char *owner;
} hyprlodr_pair;

typedef struct {
    char *name;
    char *provider;
} hyprlodr_capability;

typedef struct {
    void *library;
    const hyprlodr_plugin_descriptor *descriptor;
    char path[HYPRLODR_PATH_MAX];
    char state_dir[HYPRLODR_PATH_MAX];
    int initialized;
    int failed;
    int disabled;
} hyprlodr_loaded_plugin;

enum { HYPRLODR_UNDO_STATE = 1, HYPRLODR_UNDO_CAPABILITY = 2 };

typedef struct {
    int kind;
    char *key;
    char *previous_value;
    char *previous_owner;
} hyprlodr_undo;

struct hyprlodr_loader {
    hyprlodr_loaded_plugin plugins[HYPRLODR_MAX_PLUGINS];
    size_t plugin_count;
    size_t order[HYPRLODR_MAX_PLUGINS];
    size_t order_count;
    hyprlodr_pair state[HYPRLODR_MAX_PAIRS];
    size_t state_count;
    hyprlodr_capability capabilities[HYPRLODR_MAX_CAPABILITIES];
    size_t capability_count;
    hyprlodr_undo undo[HYPRLODR_MAX_UNDO];
    size_t undo_count;
    char dirs[HYPRLODR_MAX_DIRS][HYPRLODR_PATH_MAX];
    size_t dir_count;
    char state_root[HYPRLODR_PATH_MAX];
    char disabled[HYPRLODR_MAX_DISABLED][HYPRLODR_ID_MAX];
    size_t disabled_count;
    FILE *log;
    unsigned long seq;
    unsigned seed;
    char current_mod[HYPRLODR_ID_MAX];
    int error_count;
    char last_error[512];
    int resolved;
    int shutdown_done;
};

/* ------------------------------------------------------------------ helpers */

static char *hyprlodr_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *copy = (char *)malloc(n);
    if (!copy) return NULL;
    memcpy(copy, s, n);
    return copy;
}

static void json_string(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; ++p) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', out);
            fputc((int)*p, out);
        } else if (*p < 0x20) {
            fprintf(out, "\\u%04x", (unsigned)*p);
        } else {
            fputc((int)*p, out);
        }
    }
    fputc('"', out);
}

static void event(hyprlodr_loader *l, const char *name, const char *mod, const char *message) {
    if (!l->log) return;
    fprintf(l->log, "{\"seq\":%lu,\"event\":", ++l->seq);
    json_string(l->log, name);
    fputs(",\"mod\":", l->log);
    json_string(l->log, mod ? mod : "");
    fputs(",\"message\":", l->log);
    json_string(l->log, message ? message : "");
    fputs("}\n", l->log);
    fflush(l->log);
}

static void record_error(hyprlodr_loader *l, const char *mod, const char *message) {
    ++l->error_count;
    snprintf(l->last_error, sizeof(l->last_error), "%s: %s", mod ? mod : "loader",
             message ? message : "error");
    event(l, "error", mod, message);
    if (!l->log) fprintf(stderr, "HYPRLODR: %s\n", l->last_error);
}

/* ------------------------------------------------------------------- semver */

typedef struct { unsigned major, minor, patch; } hyprlodr_semver;

static int semver_parse(const char *s, hyprlodr_semver *out) {
    unsigned parts[3] = {0, 0, 0};
    int index = 0;
    if (!s || !*s) return -1;
    while (*s) {
        if (*s == '-') break; /* pre-release suffix is ignored for ordering */
        if (*s == '+') break; /* build metadata is ignored */
        if (*s == '.') {
            if (++index > 2) return -1;
            ++s;
            if (!*s) return -1;
            continue;
        }
        if (*s < '0' || *s > '9') return -1;
        if (parts[index] > 100000u) return -1;
        parts[index] = parts[index] * 10u + (unsigned)(*s - '0');
        ++s;
    }
    out->major = parts[0];
    out->minor = parts[1];
    out->patch = parts[2];
    return 0;
}

static int semver_cmp(hyprlodr_semver a, hyprlodr_semver b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

enum {
    HYPRLODR_OP_ANY = 0,
    HYPRLODR_OP_GE,
    HYPRLODR_OP_LE,
    HYPRLODR_OP_GT,
    HYPRLODR_OP_LT,
    HYPRLODR_OP_EQ,
    HYPRLODR_OP_CARET,
    HYPRLODR_OP_TILDE
};

/* Split a dependency spec "id@op" into its id and optional constraint. */
static int spec_parse(const char *spec, char *id, size_t idsz, int *op,
                      hyprlodr_semver *version) {
    const char *at = strchr(spec, '@');
    size_t len = at ? (size_t)(at - spec) : strlen(spec);
    if (len == 0 || len >= idsz) return -1;
    memcpy(id, spec, len);
    id[len] = '\0';
    *op = HYPRLODR_OP_ANY;
    if (!at) return 0;

    const char *cursor = at + 1;
    if (cursor[0] == '>' && cursor[1] == '=') { *op = HYPRLODR_OP_GE; cursor += 2; }
    else if (cursor[0] == '<' && cursor[1] == '=') { *op = HYPRLODR_OP_LE; cursor += 2; }
    else if (cursor[0] == '>') { *op = HYPRLODR_OP_GT; cursor += 1; }
    else if (cursor[0] == '<') { *op = HYPRLODR_OP_LT; cursor += 1; }
    else if (cursor[0] == '=') { *op = HYPRLODR_OP_EQ; cursor += 1; }
    else if (cursor[0] == '^') { *op = HYPRLODR_OP_CARET; cursor += 1; }
    else if (cursor[0] == '~') { *op = HYPRLODR_OP_TILDE; cursor += 1; }
    return semver_parse(cursor, version);
}

static int constraint_satisfied(int op, hyprlodr_semver required, const char *version) {
    hyprlodr_semver actual;
    if (op == HYPRLODR_OP_ANY) return 1;
    if (semver_parse(version, &actual) != 0) return 0;
    int cmp = semver_cmp(actual, required);
    switch (op) {
    case HYPRLODR_OP_GE: return cmp >= 0;
    case HYPRLODR_OP_LE: return cmp <= 0;
    case HYPRLODR_OP_GT: return cmp > 0;
    case HYPRLODR_OP_LT: return cmp < 0;
    case HYPRLODR_OP_EQ: return cmp == 0;
    case HYPRLODR_OP_CARET: return cmp >= 0 && actual.major == required.major;
    case HYPRLODR_OP_TILDE:
        return cmp >= 0 && actual.major == required.major && actual.minor == required.minor;
    default: return 0;
    }
}

/* --------------------------------------------------------------- host API */

static int find_plugin(const hyprlodr_loader *l, const char *id) {
    for (size_t i = 0; i < l->plugin_count; ++i)
        if (!strcmp(l->plugins[i].descriptor->id, id)) return (int)i;
    return -1;
}

static void undo_clear(hyprlodr_loader *l) {
    for (size_t i = 0; i < l->undo_count; ++i) {
        free(l->undo[i].key);
        free(l->undo[i].previous_value);
        free(l->undo[i].previous_owner);
    }
    l->undo_count = 0;
}

static int undo_push(hyprlodr_loader *l, int kind, const char *key,
                     const char *previous_value, const char *previous_owner) {
    if (l->undo_count == HYPRLODR_MAX_UNDO) return -1;
    hyprlodr_undo *u = &l->undo[l->undo_count];
    memset(u, 0, sizeof *u);
    u->kind = kind;
    u->key = hyprlodr_strdup(key);
    u->previous_value = hyprlodr_strdup(previous_value);
    u->previous_owner = hyprlodr_strdup(previous_owner);
    if (!u->key || (previous_value && !u->previous_value) ||
        (previous_owner && !u->previous_owner)) {
        free(u->key);
        free(u->previous_value);
        free(u->previous_owner);
        return -1;
    }
    ++l->undo_count;
    return 0;
}

static void api_log(void *ctx, const char *level, const char *id, const char *message) {
    hyprlodr_loader *l = (hyprlodr_loader *)ctx;
    char msg[1024];
    snprintf(msg, sizeof(msg), "%s: %s", level ? level : "info", message ? message : "");
    event(l, "log", id, msg);
}

/* Copy the owner id, tolerating an empty current mod during host-driven setup. */
static char *owner_copy(const char *owner) {
    return (owner && *owner) ? hyprlodr_strdup(owner) : NULL;
}

static int api_state_set(void *ctx, const char *key, const char *value) {
    hyprlodr_loader *l = (hyprlodr_loader *)ctx;
    if (!key || !*key || !value || strlen(key) >= HYPRLODR_ID_MAX) return -1;
    for (size_t i = 0; i < l->state_count; ++i) {
        if (strcmp(l->state[i].key, key)) continue;
        if (undo_push(l, HYPRLODR_UNDO_STATE, key, l->state[i].value, l->state[i].owner) != 0)
            return -1;
        char *next = hyprlodr_strdup(value);
        char *next_owner = owner_copy(l->current_mod);
        if (!next || ((l->current_mod[0] != '\0') && !next_owner)) {
            free(next);
            free(next_owner);
            return -1;
        }
        free(l->state[i].value);
        free(l->state[i].owner);
        l->state[i].value = next;
        l->state[i].owner = next_owner;
        return 0;
    }
    if (l->state_count == HYPRLODR_MAX_PAIRS) return -1;
    if (undo_push(l, HYPRLODR_UNDO_STATE, key, NULL, NULL) != 0) return -1;
    char *k = hyprlodr_strdup(key), *v = hyprlodr_strdup(value);
    char *owner = owner_copy(l->current_mod);
    if (!k || !v || ((l->current_mod[0] != '\0') && !owner)) {
        free(k);
        free(v);
        free(owner);
        return -1;
    }
    l->state[l->state_count].key = k;
    l->state[l->state_count].value = v;
    l->state[l->state_count].owner = owner;
    ++l->state_count;
    return 0;
}

static const char *api_state_get(void *ctx, const char *key) {
    hyprlodr_loader *l = (hyprlodr_loader *)ctx;
    if (!key) return NULL;
    for (size_t i = 0; i < l->state_count; ++i)
        if (!strcmp(l->state[i].key, key)) return l->state[i].value;
    return NULL;
}

static int api_state_remove(void *ctx, const char *key) {
    hyprlodr_loader *l = (hyprlodr_loader *)ctx;
    if (!key) return -1;
    for (size_t i = 0; i < l->state_count; ++i) {
        if (strcmp(l->state[i].key, key)) continue;
        if (undo_push(l, HYPRLODR_UNDO_STATE, key, l->state[i].value, l->state[i].owner) != 0)
            return -1;
        free(l->state[i].key);
        free(l->state[i].value);
        free(l->state[i].owner);
        memmove(&l->state[i], &l->state[i + 1],
                (l->state_count - i - 1) * sizeof l->state[0]);
        --l->state_count;
        return 0;
    }
    return -1;
}

static int api_capability_register(void *ctx, const char *name, const char *provider) {
    hyprlodr_loader *l = (hyprlodr_loader *)ctx;
    if (!name || !*name || !provider || strlen(name) >= HYPRLODR_ID_MAX) return -1;
    for (size_t i = 0; i < l->capability_count; ++i)
        if (!strcmp(l->capabilities[i].name, name)) return -1;
    if (l->capability_count == HYPRLODR_MAX_CAPABILITIES) return -1;
    if (undo_push(l, HYPRLODR_UNDO_CAPABILITY, name, NULL, NULL) != 0) return -1;
    char *n = hyprlodr_strdup(name), *p = hyprlodr_strdup(provider);
    if (!n || !p) {
        free(n);
        free(p);
        return -1;
    }
    l->capabilities[l->capability_count].name = n;
    l->capabilities[l->capability_count].provider = p;
    ++l->capability_count;
    event(l, "capability_registered", provider, name);
    return 0;
}

static int api_capability_has(void *ctx, const char *name) {
    hyprlodr_loader *l = (hyprlodr_loader *)ctx;
    if (!name) return 0;
    for (size_t i = 0; i < l->capability_count; ++i)
        if (!strcmp(l->capabilities[i].name, name)) return 1;
    return 0;
}

/* ABI 3: the directory the current mod may use for its own files. */
static const char *api_state_dir(void *ctx) {
    hyprlodr_loader *l = (hyprlodr_loader *)ctx;
    int index = find_plugin(l, l->current_mod);
    if (index < 0) return NULL;
    return l->plugins[index].state_dir[0] ? l->plugins[index].state_dir : NULL;
}

static int api_is_loaded(void *ctx, const char *mod_id) {
    hyprlodr_loader *l = (hyprlodr_loader *)ctx;
    if (!mod_id) return 0;
    int index = find_plugin(l, mod_id);
    return index >= 0 && l->plugins[index].initialized;
}

static hyprlodr_host_api host_api(hyprlodr_loader *l) {
    hyprlodr_host_api api;
    memset(&api, 0, sizeof api);
    api.struct_size = (uint32_t)sizeof api;
    api.abi_version = HYPRLODR_ABI_VERSION;
    api.context = l;
    api.log = api_log;
    api.state_set = api_state_set;
    api.state_get = api_state_get;
    api.capability_register = api_capability_register;
    api.capability_has = api_capability_has;
    api.state_remove = api_state_remove;
    api.is_loaded = api_is_loaded;
    api.state_dir = api_state_dir;
    return api;
}

/* Undo every mutation made by the mod whose initialization failed. */
static void rollback_current(hyprlodr_loader *l, const char *mod_id) {
    size_t restored = 0;
    while (l->undo_count > 0) {
        hyprlodr_undo *u = &l->undo[--l->undo_count];
        if (u->kind == HYPRLODR_UNDO_STATE) {
            for (size_t i = 0; i < l->state_count; ++i) {
                if (strcmp(l->state[i].key, u->key)) continue;
                if (u->previous_value) {
                    char *value = hyprlodr_strdup(u->previous_value);
                    char *owner = hyprlodr_strdup(u->previous_owner);
                    if (value) {
                        free(l->state[i].value);
                        l->state[i].value = value;
                    }
                    free(l->state[i].owner);
                    l->state[i].owner = owner;
                } else {
                    free(l->state[i].key);
                    free(l->state[i].value);
                    free(l->state[i].owner);
                    memmove(&l->state[i], &l->state[i + 1],
                            (l->state_count - i - 1) * sizeof l->state[0]);
                    --l->state_count;
                }
                ++restored;
                break;
            }
        } else if (u->kind == HYPRLODR_UNDO_CAPABILITY) {
            for (size_t i = 0; i < l->capability_count; ++i) {
                if (strcmp(l->capabilities[i].name, u->key)) continue;
                free(l->capabilities[i].name);
                free(l->capabilities[i].provider);
                memmove(&l->capabilities[i], &l->capabilities[i + 1],
                        (l->capability_count - i - 1) * sizeof l->capabilities[0]);
                --l->capability_count;
                ++restored;
                break;
            }
        }
        free(u->key);
        free(u->previous_value);
        free(u->previous_owner);
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "rolled back %zu state/capability change(s)", restored);
    event(l, "rollback", mod_id, msg);
}

/* --------------------------------------------------------------- discovery */

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} hyprlodr_path_list;

static int path_list_add(hyprlodr_path_list *list, const char *path) {
    if (list->count == list->capacity) {
        size_t next = list->capacity ? list->capacity * 2 : 16;
        char **items = (char **)realloc(list->items, next * sizeof *items);
        if (!items) return -1;
        list->items = items;
        list->capacity = next;
    }
    char *copy = hyprlodr_strdup(path);
    if (!copy) return -1;
    list->items[list->count++] = copy;
    return 0;
}

static void path_list_free(hyprlodr_path_list *list) {
    for (size_t i = 0; i < list->count; ++i) free(list->items[i]);
    free(list->items);
    list->items = NULL;
    list->count = list->capacity = 0;
}

static int compare_paths(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

#ifndef _WIN32
static int has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}
#endif

static int id_valid(const char *id) {
    if (!id || !*id || strlen(id) >= HYPRLODR_ID_MAX) return 0;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' ||
              *p == '-' || *p == '_'))
            return 0;
    return 1;
}

static int collect_dir(const char *dir, hyprlodr_path_list *list) {
#ifdef _WIN32
    char pattern[HYPRLODR_PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s\\*%s", dir, LIB_EXT);
    WIN32_FIND_DATAA data;
    HANDLE handle = FindFirstFileA(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char path[HYPRLODR_PATH_MAX];
        snprintf(path, sizeof(path), "%s\\%s", dir, data.cFileName);
        if (path_list_add(list, path) != 0) {
            FindClose(handle);
            return -1;
        }
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
    return 0;
#else
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *entry;
    int status = 0;
    while ((entry = readdir(d))) {
        if (!has_suffix(entry->d_name, LIB_EXT)) continue;
        char path[HYPRLODR_PATH_MAX];
        snprintf(path, sizeof(path), "%s%c%s", dir, PATH_SEP, entry->d_name);
        if (path_list_add(list, path) != 0) {
            status = -1;
            break;
        }
    }
    closedir(d);
    return status;
#endif
}

static int open_plugin(hyprlodr_loader *l, const char *path) {
#ifdef _WIN32
    HMODULE lib = LoadLibraryA(path);
    if (!lib) {
        record_error(l, path, "LoadLibrary failed");
        return -1;
    }
    void *symbol = (void *)(uintptr_t)GetProcAddress(lib, HYPRLODR_PLUGIN_ENTRY);
#else
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        /* dlerror() clears the error, so read it exactly once. */
        const char *reason = dlerror();
        record_error(l, path, reason ? reason : "dlopen failed");
        return -1;
    }
    void *symbol = dlsym(lib, HYPRLODR_PLUGIN_ENTRY);
#endif
    hyprlodr_query_fn query = NULL;
    if (symbol) {
        /* Casting object pointers to function pointers is not valid C; copy
         * the representation instead. Sizes are asserted to match. */
        _Static_assert(sizeof query == sizeof symbol, "function/object pointer size mismatch");
        memcpy(&query, &symbol, sizeof query);
    }
    if (!query) {
        record_error(l, path, "missing export " HYPRLODR_PLUGIN_ENTRY);
#ifdef _WIN32
        FreeLibrary(lib);
#else
        dlclose(lib);
#endif
        return -1;
    }

    const hyprlodr_plugin_descriptor *d = query(HYPRLODR_ABI_VERSION);
    if (!d) {
        record_error(l, path, "mod rejected host ABI version");
#ifdef _WIN32
        FreeLibrary(lib);
#else
        dlclose(lib);
#endif
        return -1;
    }
    if (d->struct_size < offsetof(hyprlodr_plugin_descriptor, conflicts)) {
        record_error(l, path, "descriptor struct_size too small");
#ifdef _WIN32
        FreeLibrary(lib);
#else
        dlclose(lib);
#endif
        return -1;
    }
    if (d->abi_version != HYPRLODR_ABI_VERSION) {
        char msg[128];
        snprintf(msg, sizeof(msg), "native ABI mismatch: mod declares %u, host %u",
                 d->abi_version, HYPRLODR_ABI_VERSION);
        record_error(l, path, msg);
#ifdef _WIN32
        FreeLibrary(lib);
#else
        dlclose(lib);
#endif
        return -1;
    }
    if (!id_valid(d->id) || !d->version || !d->initialize || !d->shutdown) {
        record_error(l, path, "invalid descriptor (id/version/callbacks)");
#ifdef _WIN32
        FreeLibrary(lib);
#else
        dlclose(lib);
#endif
        return -1;
    }
    if (d->dependency_count && !d->dependencies) {
        record_error(l, d->id, "dependency_count set without dependencies array");
#ifdef _WIN32
        FreeLibrary(lib);
#else
        dlclose(lib);
#endif
        return -1;
    }
    if (HYPRLODR_DESCRIPTOR_HAS(d, conflict_count) && d->conflict_count && !d->conflicts) {
        record_error(l, d->id, "conflict_count set without conflicts array");
#ifdef _WIN32
        FreeLibrary(lib);
#else
        dlclose(lib);
#endif
        return -1;
    }
    if (d->dependency_count > HYPRLODR_MAX_PLUGINS) {
        record_error(l, d->id, "dependency_count exceeds limit");
#ifdef _WIN32
        FreeLibrary(lib);
#else
        dlclose(lib);
#endif
        return -1;
    }
    if (find_plugin(l, d->id) >= 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "duplicate mod id (already loaded from %.180s)",
                 l->plugins[find_plugin(l, d->id)].path);
        record_error(l, d->id, msg);
#ifdef _WIN32
        FreeLibrary(lib);
#else
        dlclose(lib);
#endif
        return -1;
    }
    if (l->plugin_count == HYPRLODR_MAX_PLUGINS) {
        record_error(l, d->id, "plugin limit exceeded");
#ifdef _WIN32
        FreeLibrary(lib);
#else
        dlclose(lib);
#endif
        return -1;
    }

    hyprlodr_loaded_plugin *p = &l->plugins[l->plugin_count++];
    memset(p, 0, sizeof *p);
    p->library = lib;
    p->descriptor = d;
    snprintf(p->path, sizeof(p->path), "%s", path);
    p->disabled = 0;
    event(l, "discovered", d->id, p->path);
    return 0;
}

/* Seeded Fisher-Yates over the loaded array. Proves resolution ignores
 * discovery order. */
static void shuffle_plugins(hyprlodr_loader *l, unsigned seed) {
    unsigned state = seed * 2654435761u + 1u;
    for (size_t i = l->plugin_count; i > 1; --i) {
        state = state * 1103515245u + 12345u;
        size_t j = (size_t)((state >> 16) % i);
        hyprlodr_loaded_plugin tmp = l->plugins[i - 1];
        l->plugins[i - 1] = l->plugins[j];
        l->plugins[j] = tmp;
    }
}

static int is_disabled(const hyprlodr_loader *l, const char *id) {
    for (size_t i = 0; i < l->disabled_count; ++i)
        if (!strcmp(l->disabled[i], id)) return 1;
    return 0;
}

int hyprlodr_loader_add_dir(hyprlodr_loader *loader, const char *directory) {
    if (!loader || !directory || !*directory) return HYPRLODR_ERR_ARG;
    if (loader->dir_count == HYPRLODR_MAX_DIRS) return HYPRLODR_ERR_LIMIT;
    snprintf(loader->dirs[loader->dir_count], HYPRLODR_PATH_MAX, "%s", directory);
    ++loader->dir_count;
    return HYPRLODR_OK;
}

int hyprlodr_loader_disable(hyprlodr_loader *loader, const char *mod_id) {
    if (!loader || !mod_id || !*mod_id) return HYPRLODR_ERR_ARG;
    if (loader->disabled_count == HYPRLODR_MAX_DISABLED) return HYPRLODR_ERR_LIMIT;
    snprintf(loader->disabled[loader->disabled_count], HYPRLODR_ID_MAX, "%s", mod_id);
    ++loader->disabled_count;
    return HYPRLODR_OK;
}

int hyprlodr_loader_disable_file(hyprlodr_loader *loader, const char *path) {
    if (!loader || !path) return HYPRLODR_ERR_ARG;
    FILE *file = fopen(path, "r");
    if (!file) return HYPRLODR_OK; /* absent list means nothing is disabled */
    char line[256];
    while (fgets(line, sizeof line, file)) {
        char *start = line;
        while (*start == ' ' || *start == '\t') ++start;
        char *end = start + strlen(start);
        while (end > start && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' ||
                               end[-1] == '\t'))
            --end;
        *end = '\0';
        if (!*start || *start == '#') continue;
        (void)hyprlodr_loader_disable(loader, start);
    }
    fclose(file);
    return HYPRLODR_OK;
}

int hyprlodr_loader_set_state_root(hyprlodr_loader *loader, const char *path) {
    if (!loader || !path || !*path) return HYPRLODR_ERR_ARG;
    snprintf(loader->state_root, sizeof loader->state_root, "%s", path);
    return HYPRLODR_OK;
}

int hyprlodr_loader_set_log(hyprlodr_loader *loader, const char *path) {
    if (!loader || !path) return HYPRLODR_ERR_ARG;
    loader->log = fopen(path, "w");
    if (!loader->log) return HYPRLODR_ERR_IO;
    return HYPRLODR_OK;
}

void hyprlodr_loader_set_seed(hyprlodr_loader *loader, unsigned seed) {
    if (loader) loader->seed = seed;
}

/* ---------------------------------------------------------------- resolution */

static int compare_plugin_ids(hyprlodr_loader *l, size_t a, size_t b) {
    return strcmp(l->plugins[a].descriptor->id, l->plugins[b].descriptor->id);
}

/* Sort plugin indices by mod id so every decision and error message is
 * independent of discovery order. */
static void sorted_indices(hyprlodr_loader *l, size_t *out) {
    for (size_t i = 0; i < l->plugin_count; ++i) out[i] = i;
    for (size_t i = 1; i < l->plugin_count; ++i) {
        size_t item = out[i], j = i;
        while (j > 0 && compare_plugin_ids(l, item, out[j - 1]) < 0) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = item;
    }
}

static int check_conflicts(hyprlodr_loader *l, size_t index) {
    const hyprlodr_plugin_descriptor *d = l->plugins[index].descriptor;
    if (!HYPRLODR_DESCRIPTOR_HAS(d, conflict_count) || !d->conflict_count) return HYPRLODR_OK;
    for (size_t i = 0; i < d->conflict_count; ++i) {
        char other_id[HYPRLODR_ID_MAX];
        int op;
        hyprlodr_semver version;
        const char *spec = d->conflicts[i];
        if (!spec) continue;
        if (spec_parse(spec, other_id, sizeof other_id, &op, &version) != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "malformed conflict spec '%s'", spec);
            record_error(l, d->id, msg);
            return HYPRLODR_ERR_CONFLICT;
        }
        int other = find_plugin(l, other_id);
        if (other < 0 || l->plugins[other].disabled) continue;
        if (!constraint_satisfied(op, version, l->plugins[other].descriptor->version)) continue;
        char msg[256];
        snprintf(msg, sizeof(msg), "conflicts with loaded mod %s %s", other_id,
                 l->plugins[other].descriptor->version);
        record_error(l, d->id, msg);
        return HYPRLODR_ERR_CONFLICT;
    }
    return HYPRLODR_OK;
}

static int resolve_order(hyprlodr_loader *l) {
    size_t order_of[HYPRLODR_MAX_PLUGINS];
    sorted_indices(l, order_of);

    /* Mark disabled mods before ordering. */
    for (size_t i = 0; i < l->plugin_count; ++i) {
        hyprlodr_loaded_plugin *p = &l->plugins[i];
        if (is_disabled(l, p->descriptor->id)) {
            p->disabled = 1;
            continue;
        }
        if (check_conflicts(l, i) != HYPRLODR_OK) return HYPRLODR_ERR_CONFLICT;
    }

    unsigned char emitted[HYPRLODR_MAX_PLUGINS] = {0};
    size_t enabled = 0;
    for (size_t i = 0; i < l->plugin_count; ++i)
        if (!l->plugins[i].disabled) ++enabled;

    while (l->order_count < enabled) {
        size_t chosen = SIZE_MAX;
        for (size_t s = 0; s < l->plugin_count && chosen == SIZE_MAX; ++s) {
            size_t index = order_of[s];
            hyprlodr_loaded_plugin *p = &l->plugins[index];
            if (p->disabled || emitted[index]) continue;
            const hyprlodr_plugin_descriptor *d = p->descriptor;
            int ready = 1;
            for (size_t dep_index = 0; dep_index < d->dependency_count && ready; ++dep_index) {
                char dep_id[HYPRLODR_ID_MAX];
                int op;
                hyprlodr_semver version;
                const char *spec = d->dependencies[dep_index];
                if (!spec || spec_parse(spec, dep_id, sizeof dep_id, &op, &version) != 0) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "malformed dependency spec '%s'",
                             spec ? spec : "(null)");
                    record_error(l, d->id, msg);
                    return HYPRLODR_ERR_DEPENDENCY;
                }
                int dep = find_plugin(l, dep_id);
                if (dep < 0) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "missing dependency: %s", dep_id);
                    record_error(l, d->id, msg);
                    return HYPRLODR_ERR_DEPENDENCY;
                }
                if (l->plugins[dep].disabled) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "dependency is disabled: %s", dep_id);
                    record_error(l, d->id, msg);
                    return HYPRLODR_ERR_DEPENDENCY;
                }
                if (!constraint_satisfied(op, version, l->plugins[dep].descriptor->version)) {
                    char msg[512];
                    snprintf(msg, sizeof(msg), "dependency %.100s %.40s does not satisfy '%.200s'",
                             dep_id, l->plugins[dep].descriptor->version, spec);
                    record_error(l, d->id, msg);
                    return HYPRLODR_ERR_DEPENDENCY;
                }
                if (!emitted[dep]) ready = 0;
            }
            if (ready) chosen = index;
        }
        if (chosen == SIZE_MAX) {
            char detail[512];
            size_t used = 0;
            detail[0] = '\0';
            for (size_t s = 0; s < l->plugin_count; ++s) {
                size_t index = order_of[s];
                if (l->plugins[index].disabled || emitted[index]) continue;
                int n = snprintf(detail + used, sizeof detail - used, "%s%s",
                                 used ? ", " : "cycle among: ", l->plugins[index].descriptor->id);
                if (n < 0 || (size_t)n >= sizeof detail - used) break;
                used += (size_t)n;
            }
            record_error(l, "loader", detail);
            return HYPRLODR_ERR_CYCLE;
        }
        emitted[chosen] = 1;
        l->order[l->order_count++] = chosen;
    }
    return HYPRLODR_OK;
}

int hyprlodr_loader_resolve(hyprlodr_loader *loader) {
    hyprlodr_loader *l = loader;
    if (!l) return HYPRLODR_ERR_ARG;
    if (l->resolved) return HYPRLODR_OK;
    if (!l->dir_count) return HYPRLODR_ERR_ARG;

    hyprlodr_path_list paths;
    memset(&paths, 0, sizeof paths);
    for (size_t i = 0; i < l->dir_count; ++i) {
        if (collect_dir(l->dirs[i], &paths) != 0) {
            char msg[512];
            snprintf(msg, sizeof(msg), "cannot scan mod directory: %.400s", l->dirs[i]);
            record_error(l, "loader", msg);
            path_list_free(&paths);
            return HYPRLODR_ERR_IO;
        }
    }
    if (paths.count == 0) {
        record_error(l, "loader", "no mod libraries found in search path");
        path_list_free(&paths);
        return HYPRLODR_ERR_IO;
    }
    qsort(paths.items, paths.count, sizeof paths.items[0], compare_paths);
    event(l, "discovery_complete", "", "");
    for (size_t i = 0; i < paths.count; ++i) (void)open_plugin(l, paths.items[i]);
    path_list_free(&paths);

    if (l->plugin_count == 0) {
        l->resolved = 1;
        return HYPRLODR_ERR_DEPENDENCY;
    }
    if (l->seed) shuffle_plugins(l, l->seed);

    int status = resolve_order(l);
    l->resolved = 1;
    if (status != HYPRLODR_OK) return status;

    for (size_t i = 0; i < l->order_count; ++i) {
        const hyprlodr_plugin_descriptor *d = l->plugins[l->order[i]].descriptor;
        event(l, "resolved", d->id, d->version);
    }
    return HYPRLODR_OK;
}

/* ------------------------------------------------------------------ lifecycle */

static void shutdown_initialized(hyprlodr_loader *l) {
    hyprlodr_host_api api = host_api(l);
    for (size_t i = l->order_count; i > 0; --i) {
        hyprlodr_loaded_plugin *p = &l->plugins[l->order[i - 1]];
        if (!p->initialized) continue;
        event(l, "shutdown", p->descriptor->id, "");
        snprintf(l->current_mod, sizeof l->current_mod, "%s", p->descriptor->id);
        p->descriptor->shutdown(&api);
        l->current_mod[0] = '\0';
        p->initialized = 0;
    }
}

/* Create one directory. An existing directory is success: mods may be enabled
 * across several sessions and the loader must not care. */
static int ensure_dir(const char *path) {
#ifdef _WIN32
    /* Fails with ERROR_ALREADY_EXISTS for an existing path, which is fine. The
     * mod's own file operations are the real verification. */
    CreateDirectoryA(path, NULL);
    return 0;
#else
    if (mkdir(path, 0777) == 0) return 0;
    return errno == EEXIST ? 0 : -1;
#endif
}

/* Reserve <state_root>/<mod-id> before the mod can ask for it. A failure is
 * reported and leaves state_dir() NULL; the mod decides whether it can cope. */
static void prepare_state_dir(hyprlodr_loader *l, hyprlodr_loaded_plugin *p) {
    const char *id = p->descriptor->id;
    p->state_dir[0] = '\0';
    if (!l->state_root[0]) return;
    if (ensure_dir(l->state_root) != 0) {
        event(l, "state_dir_failed", id, l->state_root);
        return;
    }
    size_t root_length = strlen(l->state_root);
    size_t id_length = strlen(id);
    if (root_length + 1u + id_length + 1u > sizeof p->state_dir) {
        event(l, "state_dir_failed", id, "path too long");
        return;
    }
    memcpy(p->state_dir, l->state_root, root_length);
    p->state_dir[root_length] = PATH_SEP;
    memcpy(p->state_dir + root_length + 1u, id, id_length + 1u);
    if (ensure_dir(p->state_dir) != 0) {
        event(l, "state_dir_failed", id, p->state_dir);
        p->state_dir[0] = '\0';
        return;
    }
    event(l, "state_dir", id, p->state_dir);
}

int hyprlodr_loader_init(hyprlodr_loader *loader) {
    hyprlodr_loader *l = loader;
    if (!l) return HYPRLODR_ERR_ARG;
    if (!l->resolved) return HYPRLODR_ERR_ARG;

    hyprlodr_host_api api = host_api(l);
    for (size_t i = 0; i < l->order_count; ++i) {
        hyprlodr_loaded_plugin *p = &l->plugins[l->order[i]];
        const hyprlodr_plugin_descriptor *d = p->descriptor;
        event(l, "initialize_begin", d->id, d->version);
        prepare_state_dir(l, p);
        undo_clear(l);
        snprintf(l->current_mod, sizeof l->current_mod, "%s", d->id);
        int rc = d->initialize(&api);
        l->current_mod[0] = '\0';
        if (rc != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "initialization failed (code %d); dependent startup stopped", rc);
            p->failed = 1;
            rollback_current(l, d->id);
            l->order_count = i; /* keep only successfully initialized mods */
            shutdown_initialized(l);
            record_error(l, d->id, msg);
            return HYPRLODR_ERR_INIT;
        }
        p->initialized = 1;
        undo_clear(l);
        event(l, "initialize_ok", d->id, "");
    }
    return HYPRLODR_OK;
}

void hyprlodr_loader_shutdown(hyprlodr_loader *loader) {
    if (!loader || loader->shutdown_done) return;
    loader->shutdown_done = 1;
    shutdown_initialized(loader);
    event(loader, "shutdown_complete", "", "");
}

hyprlodr_loader *hyprlodr_loader_create(void) {
    hyprlodr_loader *l = (hyprlodr_loader *)calloc(1, sizeof *l);
    return l;
}

int hyprlodr_loader_run(hyprlodr_loader *l, const char *plugin_directory,
                        const char *event_log_path) {
    if (!l || !plugin_directory) return HYPRLODR_ERR_ARG;
    int status;
    if ((status = hyprlodr_loader_add_dir(l, plugin_directory)) != HYPRLODR_OK) return status;
    if (event_log_path && (status = hyprlodr_loader_set_log(l, event_log_path)) != HYPRLODR_OK)
        return status;
    if ((status = hyprlodr_loader_resolve(l)) != HYPRLODR_OK) return status;
    return hyprlodr_loader_init(l);
}

void hyprlodr_loader_destroy(hyprlodr_loader *l) {
    if (!l) return;
    hyprlodr_loader_shutdown(l);
    for (size_t i = 0; i < l->plugin_count; ++i) {
        if (!l->plugins[i].library) continue;
#ifdef _WIN32
        FreeLibrary((HMODULE)l->plugins[i].library);
#else
        dlclose(l->plugins[i].library);
#endif
        l->plugins[i].library = NULL;
    }
    for (size_t i = 0; i < l->state_count; ++i) {
        free(l->state[i].key);
        free(l->state[i].value);
        free(l->state[i].owner);
    }
    for (size_t i = 0; i < l->capability_count; ++i) {
        free(l->capabilities[i].name);
        free(l->capabilities[i].provider);
    }
    undo_clear(l);
    if (l->log) fclose(l->log);
    free(l);
}

size_t hyprlodr_loader_describe_all(const hyprlodr_loader *l, hyprlodr_mod_info *out,
                                    size_t capacity) {
    size_t sorted[HYPRLODR_MAX_PLUGINS];
    size_t count;
    if (!l || !out || capacity == 0) return 0;
    count = l->plugin_count < capacity ? l->plugin_count : capacity;
    for (size_t i = 0; i < l->plugin_count; ++i) sorted[i] = i;
    for (size_t i = 1; i < l->plugin_count; ++i) {
        size_t item = sorted[i], j = i;
        while (j > 0 && strcmp(l->plugins[item].descriptor->id,
                               l->plugins[sorted[j - 1]].descriptor->id) < 0) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = item;
    }
    for (size_t i = 0; i < count; ++i) {
        const hyprlodr_loaded_plugin *p = &l->plugins[sorted[i]];
        const hyprlodr_plugin_descriptor *d = p->descriptor;
        out[i].id = d->id;
        out[i].version = d->version;
        out[i].library_path = p->path;
        out[i].dependencies = d->dependencies;
        out[i].dependency_count = d->dependency_count;
        out[i].conflicts = HYPRLODR_DESCRIPTOR_HAS(d, conflicts) ? d->conflicts : NULL;
        out[i].conflict_count = HYPRLODR_DESCRIPTOR_HAS(d, conflict_count) ? d->conflict_count : 0;
        out[i].flags = HYPRLODR_DESCRIPTOR_HAS(d, flags) ? d->flags : 0u;
        out[i].disabled = p->disabled;
    }
    return count;
}

size_t hyprlodr_loader_order(const hyprlodr_loader *l, const char **ids, size_t capacity) {
    if (!l) return 0;
    for (size_t i = 0; i < l->order_count && i < capacity; ++i)
        ids[i] = l->plugins[l->order[i]].descriptor->id;
    return l->order_count;
}

size_t hyprlodr_loader_mod_count(const hyprlodr_loader *l) {
    return l ? l->plugin_count : 0;
}

int hyprlodr_loader_error_count(const hyprlodr_loader *l) {
    return l ? l->error_count : 0;
}

const char *hyprlodr_loader_last_error(const hyprlodr_loader *l) {
    return l ? l->last_error : "";
}

int hyprlodr_loader_had_error(const hyprlodr_loader *l) {
    return !l || l->error_count > 0;
}

const char *hyprlodr_loader_strerror(int status) {
    switch (status) {
    case HYPRLODR_OK: return "ok";
    case HYPRLODR_ERR_ARG: return "invalid argument";
    case HYPRLODR_ERR_IO: return "i/o error";
    case HYPRLODR_ERR_DEPENDENCY: return "dependency error";
    case HYPRLODR_ERR_CYCLE: return "dependency cycle";
    case HYPRLODR_ERR_CONFLICT: return "mod conflict";
    case HYPRLODR_ERR_INIT: return "initialization failure";
    case HYPRLODR_ERR_LIMIT: return "limit exceeded";
    default: return "unknown error";
    }
}
