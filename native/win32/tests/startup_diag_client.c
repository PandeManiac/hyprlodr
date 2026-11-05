#define HYPRLODR_MINIMAL_WIN32 1
#include "hypr_win32.h"
#include "startup_diag.h"
#include "record_route_hook.h"

#include <string.h>

static volatile LONG g_original_calls;
static volatile LONG g_callback_errors;
static hyprlodr_game_route_binding g_binding;
static hyprlodr_call_hook g_route_hook = HYPRLODR_CALL_HOOK_INITIALIZER;
static const hyprlodr_record_route g_route = {
    "hand2", 295758784u, 21972u, 1u, 295800000u, 22056u, 400000000u
};
static volatile LONG g_route_expected;
static unsigned char *g_record;
static unsigned char *g_name;

static char *find_last_separator(char *text) {
    char *result = NULL;
    for (char *at = text; *at; ++at)
        if (*at == '\\') result = at;
    return result;
}

static uintptr_t original_loader(void *context, void *record,
                                const void *name, uint32_t flags) {
    (void)context;
    (void)name;
    (void)flags;
    uint32_t tag, offset, size;
    memcpy(&offset, (unsigned char *)record + HYPRLODR_RECORD_OFFSET_FIELD, sizeof offset);
    memcpy(&size, (unsigned char *)record + HYPRLODR_RECORD_SIZE_FIELD, sizeof size);
    memcpy(&tag, (unsigned char *)record + HYPRLODR_RECORD_TAG_FIELD, sizeof tag);
    if ((tag == 1u && __atomic_load_n(&g_route_expected, __ATOMIC_ACQUIRE) &&
         (offset != g_route.replacement_offset || size != g_route.replacement_size)) ||
        (tag == 1u && !__atomic_load_n(&g_route_expected, __ATOMIC_ACQUIRE) &&
         (offset != g_route.original_offset || size != g_route.original_size)) ||
        (tag == 2u && (offset != 1234u || size != 88u)))
        __atomic_add_fetch(&g_callback_errors, 1l, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&g_original_calls, 1l, __ATOMIC_SEQ_CST);
    uintptr_t parsed = (uintptr_t)0xfeedbeef12345678ull;
    memcpy((unsigned char *)record + HYPRLODR_RECORD_OBJECT_FIELD,
           &parsed, sizeof parsed);
    return (uintptr_t)0x12345678u;
}

static void make_record(unsigned char record[HYPRLODR_GAME_RECORD_SIZE],
                        uint32_t tag, uint32_t offset, uint32_t size) {
    memset(record, 0, HYPRLODR_GAME_RECORD_SIZE);
    memcpy(record + HYPRLODR_RECORD_OFFSET_FIELD, &offset, sizeof offset);
    memcpy(record + HYPRLODR_RECORD_SIZE_FIELD, &size, sizeof size);
    memcpy(record + HYPRLODR_RECORD_TAG_FIELD, &tag, sizeof tag);
}

static void make_name(unsigned char name[32]) {
    uint64_t size = 5u, capacity = 15u;
    memset(name, 0, 32u);
    memcpy(name, "hand2", 5u);
    memcpy(name + 0x10u, &size, sizeof size);
    memcpy(name + 0x18u, &capacity, sizeof capacity);
}

static void emit_worker_events(void) {
    hyprlodr_startup_diag_emit(HYPRLODR_DIAG_ARCHIVE_ATTEMPT,
        0u, 0u, 0u, 1u, 0u, 0u, 0u, "fixture worker archive attempt");
    hyprlodr_startup_diag_emit(HYPRLODR_DIAG_ARCHIVE_OPEN,
        0u, 0u, 0u, 1u, 2u, 0u, 0u, "fixture original archive opened");
}

static DWORD WINAPI worker(LPVOID unused) {
    (void)unused;
    emit_worker_events();
    make_record(g_record, 1u, 295758784u, 21972u);
    if (hyprlodr_record_route_hook_call(NULL, g_record, g_name, 9u) != 0x12345678u)
        __atomic_add_fetch(&g_callback_errors, 1l, __ATOMIC_SEQ_CST);
    return 0;
}

static int output_path(char path[MAX_PATH]) {
    DWORD length = GetModuleFileNameA(NULL, path, (DWORD)MAX_PATH);
    if (!length || length >= MAX_PATH) return 0;
    char *separator = find_last_separator(path);
    if (!separator) return 0;
    separator[1] = '\0';
    const char suffix[] = "startup-diag.jsonl";
    if (strlen(path) + sizeof suffix > MAX_PATH) return 0;
    memcpy(path + strlen(path), suffix, sizeof suffix);
    return 1;
}

static int normal_run(const char *path) {
    unsigned char name[32], record[HYPRLODR_GAME_RECORD_SIZE];
    uint64_t run_id = 0x0000001234567890ull;
    hyprlodr_startup_diag_set_run_id(run_id);
    hyprlodr_startup_diag_enable(1);

    /* Process-attach event exists before any file or writer is available. */
    hyprlodr_startup_diag_emit_once(HYPRLODR_DIAG_EARLY_HOOK,
        0u, 0u, 0u, 0u, 1u, 0u, 0u, "rejected_callsite_mismatch");
    unsigned char bad_site[5] = {0, 0, 0, 0, 0};
    const unsigned char expected[5] = {0xe8u, 1u, 0u, 0u, 0u};
    hyprlodr_call_hook failed_hook = HYPRLODR_CALL_HOOK_INITIALIZER;
    hyprlodr_call_hook_status install_status = hyprlodr_call_hook_install(
        &failed_hook, bad_site, expected, NULL,
        (void *)hyprlodr_record_route_hook_call,
        HYPRLODR_CALL_HOOK_INSTALL_QUIESCENT, NULL);
    if (install_status != HYPRLODR_CALL_HOOK_BYTES_MISMATCH || failed_hook.installed)
        return 10;

    hyprlodr_record_route_hook_set_original_target((void *)original_loader);
    hyprlodr_record_route_hook_bind(&g_binding, NULL, NULL);
    make_name(name);
    make_record(record, 2u, 1234u, 88u);
    if (hyprlodr_record_route_hook_call(NULL, record, name, 7u) != 0x12345678u)
        return 11;
    g_record = record;
    g_name = name;

    if (__atomic_load_n(&g_original_calls, __ATOMIC_SEQ_CST) != 1l) return 13;
    if (hyprlodr_startup_diag_start(path) != 0 ||
        hyprlodr_startup_diag_start(path) != 0) return 14;
    hyprlodr_startup_diag_emit(HYPRLODR_DIAG_ROUTE_PREFLIGHT,
        0u, 0u, 0u, 1u, 0u, 0u, 0u, "manifest_not_requested");
    hyprlodr_startup_diag_emit(HYPRLODR_DIAG_ROUTE_ACTIVATION,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, "route_disabled");

    hyprlodr_record_route_hook_bind(&g_binding, &g_route_hook, &g_route);
    __atomic_store_n(&g_route_expected, 1l, __ATOMIC_RELEASE);
    /* This fixture targets the wrapper's event and route-state behavior. The
     * separate callsite fixture exercises actual instruction installation. */
    g_route_hook.installed = 1;
    hyprlodr_startup_diag_emit(HYPRLODR_DIAG_ROUTE_ACTIVATION,
        0u, 0u, 0u, 1u, 1u, 0u, 0u, "enabled");
    hyprlodr_call_hook_set_enabled(&g_route_hook, 1);

    HANDLE producer = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!producer || WaitForSingleObject(producer, 5000u) != 0u) return 15;
    CloseHandle(producer);
    uint32_t restored_offset, restored_size;
    memcpy(&restored_offset, g_record + HYPRLODR_RECORD_OFFSET_FIELD,
           sizeof restored_offset);
    memcpy(&restored_size, g_record + HYPRLODR_RECORD_SIZE_FIELD, sizeof restored_size);
    if (restored_offset != g_route.original_offset || restored_size != g_route.original_size)
        return 19;
    make_record(record, 2u, 1234u, 88u);
    if (hyprlodr_record_route_hook_call(NULL, record, name, 11u) != 0x12345678u)
        return 20;
    if (__atomic_load_n(&g_callback_errors, __ATOMIC_SEQ_CST) != 0l) return 21;
    for (unsigned i = 0; i < 100u; ++i) hyprlodr_startup_diag_drain();
    /* Last event is written by the testable safe shutdown/final-drain API. */
    hyprlodr_startup_diag_emit(HYPRLODR_DIAG_TARGET_LOADER_RETURN,
        1u, 295758784u, 21972u, 1u, 64u, 0x12345678u,
        (uintptr_t)0xfeedbeef12345678ull, "fixture final parsed object");
    if (hyprlodr_startup_diag_finish(5000u) != 0) return 16;
    if (hyprlodr_startup_diag_start(path) == 0) return 18;
    if (!g_record || __atomic_load_n(&g_original_calls, __ATOMIC_SEQ_CST) != 3l)
        return 17;
    return 0;
}

static int writer_failure_run(const char *path) {
    hyprlodr_startup_diag_set_run_id(0x0000001234567891ull);
    hyprlodr_startup_diag_enable(1);
    hyprlodr_startup_diag_emit(HYPRLODR_DIAG_EARLY_HOOK,
        0u, 0u, 0u, 0u, 1u, 0u, 0u, "installed_dormant");
    hyprlodr_startup_diag_test_fail_next_writer();
    if (hyprlodr_startup_diag_start(path) == 0) return 20;
    if (hyprlodr_startup_diag_writer_status() != 3) return 21;
    return hyprlodr_startup_diag_finish(5000u) == 0 ? 0 : 22;
}

static int overflow_run(const char *path) {
    hyprlodr_startup_diag_set_run_id(0x0000001234567892ull);
    hyprlodr_startup_diag_enable(1);
    if (hyprlodr_startup_diag_start(path) != 0) return 30;
    for (unsigned i = 0; i < 1100u; ++i)
        hyprlodr_startup_diag_emit(HYPRLODR_DIAG_ARCHIVE_ATTEMPT,
            0u, i, 1u, 0u, 0u, 0u, 0u, "overflow fixture event");
    if (hyprlodr_startup_diag_dropped() == 0u) return 31;
    return hyprlodr_startup_diag_finish(5000u) == 0 ? 0 : 32;
}

int main(int argc, char **argv) {
    char path[MAX_PATH];
    if (!output_path(path)) return 1;
    const char *mode = argc > 1 ? argv[1] : "normal";
    if (strcmp(mode, "writer-fail") == 0) return writer_failure_run(path);
    if (strcmp(mode, "overflow") == 0) return overflow_run(path);
    return normal_run(path);
}
