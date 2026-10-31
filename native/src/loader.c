#define _POSIX_C_SOURCE 200809L
#include "loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  define PATH_SEP '\\'
#  define LIB_EXT ".dll"
#else
#  include <dirent.h>
#  include <dlfcn.h>
#  include <sys/stat.h>
#  define PATH_SEP '/'
#  ifdef __APPLE__
#    define LIB_EXT ".dylib"
#  else
#    define LIB_EXT ".so"
#  endif
#endif

#define MAX_PLUGINS 128
#define MAX_PAIRS 256
#define MAX_CAPABILITIES 128
#define ID_SIZE 128

typedef struct { char *key; char *value; } pair;
typedef struct { char *name; char *provider; } capability;
typedef struct {
    void *library;
    const hyprlodr_plugin_descriptor *descriptor;
    char path[1024];
    int initialized;
    int failed;
} loaded_plugin;

struct hyprlodr_loader {
    loaded_plugin plugins[MAX_PLUGINS];
    size_t plugin_count;
    size_t order[MAX_PLUGINS];
    size_t order_count;
    pair state[MAX_PAIRS];
    size_t state_count;
    capability capabilities[MAX_CAPABILITIES];
    size_t capability_count;
    FILE *log;
    int error;
    int shutdown_done;
};

static void json_string(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; ++p) {
        if (*p == '"' || *p == '\\') { fputc('\\', out); fputc(*p, out); }
        else if (*p < 0x20) fprintf(out, "\\u%04x", *p);
        else fputc(*p, out);
    }
    fputc('"', out);
}

static void event(hyprlodr_loader *l, const char *event_name, const char *id, const char *message) {
    if (!l->log) return;
    fputs("{\"event\":", l->log); json_string(l->log, event_name);
    fputs(",\"mod\":", l->log); json_string(l->log, id ? id : "");
    fputs(",\"message\":", l->log); json_string(l->log, message ? message : "");
    fputs("}\n", l->log); fflush(l->log);
}

static void api_log(void *ctx, const char *level, const char *id, const char *message) {
    hyprlodr_loader *l = ctx;
    char msg[1024];
    snprintf(msg, sizeof(msg), "%s: %s", level ? level : "info", message ? message : "");
    event(l, "log", id, msg);
}

static int api_state_set(void *ctx, const char *key, const char *value) {
    hyprlodr_loader *l = ctx;
    if (!key || !*key || !value) return -1;
    for (size_t i = 0; i < l->state_count; ++i) if (!strcmp(l->state[i].key, key)) {
        char *next = strdup(value); if (!next) return -1;
        free(l->state[i].value); l->state[i].value = next; return 0;
    }
    if (l->state_count == MAX_PAIRS) return -1;
    char *k = strdup(key), *v = strdup(value);
    if (!k || !v) { free(k); free(v); return -1; }
    l->state[l->state_count++] = (pair){k, v}; return 0;
}

static const char *api_state_get(void *ctx, const char *key) {
    hyprlodr_loader *l = ctx;
    for (size_t i = 0; i < l->state_count; ++i) if (!strcmp(l->state[i].key, key)) return l->state[i].value;
    return NULL;
}

static int api_capability_register(void *ctx, const char *name, const char *provider) {
    hyprlodr_loader *l = ctx;
    if (!name || !*name || !provider) return -1;
    for (size_t i = 0; i < l->capability_count; ++i) if (!strcmp(l->capabilities[i].name, name)) return -1;
    if (l->capability_count == MAX_CAPABILITIES) return -1;
    char *n = strdup(name), *p = strdup(provider);
    if (!n || !p) { free(n); free(p); return -1; }
    l->capabilities[l->capability_count++] = (capability){n, p};
    event(l, "capability_registered", provider, name); return 0;
}

static int api_capability_has(void *ctx, const char *name) {
    hyprlodr_loader *l = ctx;
    for (size_t i = 0; i < l->capability_count; ++i) if (!strcmp(l->capabilities[i].name, name)) return 1;
    return 0;
}

static hyprlodr_host_api host_api(hyprlodr_loader *l) {
    return (hyprlodr_host_api){sizeof(hyprlodr_host_api), HYPRLODR_ABI_VERSION, l,
        api_log, api_state_set, api_state_get, api_capability_register, api_capability_has};
}

static void loader_error(hyprlodr_loader *l, const char *id, const char *message) {
    l->error = 1; event(l, "error", id, message); fprintf(stderr, "HYPRLODR: %s: %s\n", id, message);
}

static int has_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix); return n >= m && strcmp(s + n - m, suffix) == 0;
}

static int plugin_compare(const void *a, const void *b, void *ctx) {
    hyprlodr_loader *l = ctx;
    size_t ia = *(const size_t *)a, ib = *(const size_t *)b;
    return strcmp(l->plugins[ia].descriptor->id, l->plugins[ib].descriptor->id);
}

static void sort_ids(hyprlodr_loader *l, size_t *ids, size_t count) {
    for (size_t i = 1; i < count; ++i) {
        size_t item = ids[i], j = i;
        while (j && plugin_compare(&item, &ids[j - 1], l) < 0) { ids[j] = ids[j - 1]; --j; }
        ids[j] = item;
    }
}

static int id_valid(const char *id) {
    if (!id || !*id || strlen(id) >= ID_SIZE) return 0;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_' )) return 0;
    return 1;
}

static int open_plugin(hyprlodr_loader *l, const char *path) {
#ifdef _WIN32
    HMODULE lib = LoadLibraryA(path);
    if (!lib) return -1;
    hyprlodr_query_fn query = (hyprlodr_query_fn)(void *)GetProcAddress(lib, HYPRLODR_PLUGIN_ENTRY);
#else
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen %s: %s\n", path, dlerror()); return -1; }
    void *symbol = dlsym(lib, HYPRLODR_PLUGIN_ENTRY);
    hyprlodr_query_fn query = NULL;
    _Static_assert(sizeof query == sizeof symbol,
                   "Function pointer size mismatch");
    memcpy(&query, &symbol, sizeof query);
#endif
    if (!query) {
#ifdef _WIN32
        FreeLibrary(lib);
#else
        dlclose(lib);
#endif
        return -1;
    }
    const hyprlodr_plugin_descriptor *d = query(HYPRLODR_ABI_VERSION);
    if (!d || d->struct_size < sizeof(*d) || d->abi_version != HYPRLODR_ABI_VERSION || !id_valid(d->id) ||
        !d->version || !d->initialize || !d->shutdown || d->dependency_count > MAX_PLUGINS ||
        (d->dependency_count && !d->dependencies)) {
        loader_error(l, path, "invalid descriptor or native ABI version");
#ifdef _WIN32
        FreeLibrary(lib);
#else
        dlclose(lib);
#endif
        return -1;
    }
    for (size_t i = 0; i < l->plugin_count; ++i) if (!strcmp(l->plugins[i].descriptor->id, d->id)) {
        loader_error(l, d->id, "duplicate mod id");
#ifdef _WIN32
        FreeLibrary(lib);
#else
        dlclose(lib);
#endif
        return -1;
    }
    if (l->plugin_count == MAX_PLUGINS) {
        loader_error(l, d->id, "plugin limit exceeded");
#ifdef _WIN32
        FreeLibrary(lib);
#else
        dlclose(lib);
#endif
        return -1;
    }
    loaded_plugin *p = &l->plugins[l->plugin_count++];
    p->library = lib; p->descriptor = d; snprintf(p->path, sizeof(p->path), "%s", path);
    return 0;
}

static int discover(hyprlodr_loader *l, const char *dir) {
#ifdef _WIN32
    char pattern[1024]; snprintf(pattern, sizeof(pattern), "%s\\*%s", dir, LIB_EXT);
    WIN32_FIND_DATAA data; HANDLE h = FindFirstFileA(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) { loader_error(l, dir, "no matching plugin files"); return -1; }
    do { if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        char path[1024]; snprintf(path, sizeof(path), "%s\\%s", dir, data.cFileName); (void)open_plugin(l, path);
    }} while (FindNextFileA(h, &data)); FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) { loader_error(l, dir, "cannot open plugin directory"); return -1; }
    struct dirent *entry;
    while ((entry = readdir(d))) if (has_suffix(entry->d_name, LIB_EXT)) {
        char path[1024]; snprintf(path, sizeof(path), "%s%c%s", dir, PATH_SEP, entry->d_name); (void)open_plugin(l, path);
    }
    closedir(d);
#endif
    return l->plugin_count ? 0 : -1;
}

static size_t find_plugin(hyprlodr_loader *l, const char *id) {
    for (size_t i = 0; i < l->plugin_count; ++i) if (!strcmp(l->plugins[i].descriptor->id, id)) return i;
    return SIZE_MAX;
}

static int resolve(hyprlodr_loader *l) {
    unsigned char emitted[MAX_PLUGINS] = {0};
    while (l->order_count < l->plugin_count) {
        size_t eligible[MAX_PLUGINS], count = 0;
        for (size_t i = 0; i < l->plugin_count; ++i) if (!emitted[i]) {
            const hyprlodr_plugin_descriptor *d = l->plugins[i].descriptor;
            int ready = 1;
            for (size_t j = 0; j < d->dependency_count; ++j) {
                size_t dep = find_plugin(l, d->dependencies[j]);
                if (dep == SIZE_MAX) { char msg[256]; snprintf(msg, sizeof(msg), "missing dependency: %s", d->dependencies[j]); loader_error(l, d->id, msg); return -1; }
                if (!emitted[dep]) ready = 0;
            }
            if (ready) eligible[count++] = i;
        }
        if (!count) { loader_error(l, "loader", "dependency cycle detected"); return -1; }
        sort_ids(l, eligible, count);
        size_t chosen = eligible[0]; emitted[chosen] = 1; l->order[l->order_count++] = chosen;
    }
    return 0;
}

hyprlodr_loader *hyprlodr_loader_create(void) { return calloc(1, sizeof(hyprlodr_loader)); }

int hyprlodr_loader_run(hyprlodr_loader *l, const char *plugin_directory, const char *event_log_path) {
    if (!l || !plugin_directory || !event_log_path || l->log) return -1;
    l->log = fopen(event_log_path, "w");
    if (!l->log) { perror("event log"); return -1; }
    if (discover(l, plugin_directory) || resolve(l)) return -1;
    hyprlodr_host_api api = host_api(l);
    for (size_t i = 0; i < l->order_count; ++i) {
        loaded_plugin *p = &l->plugins[l->order[i]];
        event(l, "initialize_begin", p->descriptor->id, p->descriptor->version);
        if (p->descriptor->initialize(&api) != 0) {
            p->failed = 1; loader_error(l, p->descriptor->id, "initialization failed; dependent startup stopped"); return -1;
        }
        p->initialized = 1; event(l, "initialize_ok", p->descriptor->id, "");
    }
    return 0;
}

void hyprlodr_loader_shutdown(hyprlodr_loader *l) {
    if (!l || l->shutdown_done) return;
    l->shutdown_done = 1; hyprlodr_host_api api = host_api(l);
    for (size_t i = l->order_count; i > 0; --i) {
        loaded_plugin *p = &l->plugins[l->order[i - 1]];
        if (p->initialized) { event(l, "shutdown", p->descriptor->id, ""); p->descriptor->shutdown(&api); p->initialized = 0; }
    }
}

int hyprlodr_loader_had_error(const hyprlodr_loader *l) { return !l || l->error; }

void hyprlodr_loader_destroy(hyprlodr_loader *l) {
    if (!l) return;
    hyprlodr_loader_shutdown(l);
    for (size_t i = 0; i < l->plugin_count; ++i) if (l->plugins[i].library) {
#ifdef _WIN32
        FreeLibrary((HMODULE)l->plugins[i].library);
#else
        dlclose(l->plugins[i].library);
#endif
    }
    for (size_t i = 0; i < l->state_count; ++i) { free(l->state[i].key); free(l->state[i].value); }
    for (size_t i = 0; i < l->capability_count; ++i) { free(l->capabilities[i].name); free(l->capabilities[i].provider); }
    if (l->log) {
        fclose(l->log);
    }
    free(l);
}
