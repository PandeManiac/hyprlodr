/* Stand-in host for exercising the real example.hand-hook initialization,
 * delayed dispatch readiness and shutdown without launching the game. */
#include "hyprlodr/plugin.h"
#include "hypr_win32.h"

#include <stdio.h>
#include <string.h>

typedef struct fixture_context {
    void *slot;
    void *original;
    unsigned reads;
    unsigned ready_after;
    volatile LONG writes;
    unsigned restores;
    const char *state_directory;
} fixture_context;

static void WINAPI fixture_draw(unsigned mode, int count, unsigned type, const void *indices) {
    (void)mode; (void)count; (void)type; (void)indices;
}

static void fixture_log(void *opaque, const char *level, const char *mod,
                        const char *message) {
    (void)opaque;
    printf("mod-log: %s %s %s\n", level, mod, message);
}

static int fixture_build_matches(void *opaque) {
    (void)opaque;
    return 1;
}

static int fixture_image_read(void *opaque, uint64_t offset, void *destination,
                              size_t length) {
    fixture_context *context = (fixture_context *)opaque;
    if (offset != 0x3d9ac8ull || length != sizeof(void *)) return -1;
    if (context->slot) {
        memcpy(destination, &context->slot, length);
    } else if (context->reads++ >= context->ready_after) {
        context->slot = context->original;
        memcpy(destination, &context->slot, length);
    } else {
        void *empty = NULL;
        memcpy(destination, &empty, length);
    }
    return 0;
}

static int fixture_image_write(void *opaque, uint64_t offset, const void *source,
                               size_t length) {
    fixture_context *context = (fixture_context *)opaque;
    if (offset != 0x3d9ac8ull || length != sizeof(void *)) return -1;
    memcpy(&context->slot, source, length);
    __atomic_store_n(&context->writes, 1, __ATOMIC_RELEASE);
    return 0;
}

static size_t fixture_image_restore(void *opaque) {
    fixture_context *context = (fixture_context *)opaque;
    context->slot = context->original;
    ++context->restores;
    return sizeof(void *);
}

static const char *fixture_state_dir(void *opaque) {
    fixture_context *context = (fixture_context *)opaque;
    return context->state_directory;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("usage: hand_hook_client.exe <mod.dll> <state-dir>\n");
        return 2;
    }
    fixture_context context;
    memset(&context, 0, sizeof context);
    context.original = (void *)&fixture_draw;
    context.ready_after = 5;
    context.state_directory = argv[2];

    hyprlodr_host_api host;
    memset(&host, 0, sizeof host);
    host.struct_size = sizeof host;
    host.abi_version = HYPRLODR_ABI_VERSION;
    host.context = &context;
    host.log = fixture_log;
    host.state_dir = fixture_state_dir;
    host.build_matches = fixture_build_matches;
    host.image_read = fixture_image_read;
    host.image_write = fixture_image_write;
    host.image_restore = fixture_image_restore;

    HMODULE module = LoadLibraryA(argv[1]);
    if (!module) {
        printf("fixture: LoadLibraryA failed\n");
        return 3;
    }
    hyprlodr_query_fn query = (hyprlodr_query_fn)GetProcAddress(module,
                                                               HYPRLODR_PLUGIN_ENTRY);
    const hyprlodr_plugin_descriptor *descriptor = query ? query(HYPRLODR_ABI_VERSION) : NULL;
    if (!descriptor || descriptor->initialize(&host) != 0) {
        printf("fixture: plugin initialization failed\n");
        return 4;
    }
    for (unsigned i = 0; i < 200 &&
         __atomic_load_n(&context.writes, __ATOMIC_ACQUIRE) == 0; ++i) Sleep(5);
    if (__atomic_load_n(&context.writes, __ATOMIC_ACQUIRE) != 1 ||
        context.slot == context.original) {
        printf("fixture: dispatch was not installed after delayed readiness\n");
        descriptor->shutdown(&host);
        return 5;
    }
    printf("fixture: delayed executable dispatch installed\n");
    descriptor->shutdown(&host);
    if (context.slot != context.original || context.restores != 1) {
        printf("fixture: dispatch cleanup did not restore original\n");
        return 6;
    }
    printf("fixture: dispatch restored; capture writer stopped cleanly\n");
    FreeLibrary(module);
    return 0;
}
