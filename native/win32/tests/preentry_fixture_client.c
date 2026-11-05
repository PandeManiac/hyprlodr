#define HYPRLODR_MINIMAL_WIN32 1
#include "hypr_win32.h"
#include "hyprlodr/record_route.h"

#include <stdio.h>
#include <string.h>

typedef uintptr_t (*callsite_fn)(void *, void *, const void *, uint32_t);

#if !defined(HYPRLODR_FIXTURE_DYNAMIC)
int startup_fixture_hook_status(void);
void startup_fixture_enable(void);
void startup_fixture_disable(void);
void *startup_fixture_hook_site(void);
#endif

static volatile LONG g_loader_calls;
static volatile uint32_t g_seen_offset;
static volatile uint32_t g_seen_size;

__declspec(dllexport) uintptr_t startup_fixture_original(void *context, void *record,
                                                         const void *name,
                                                         uint32_t flags) {
    (void)name;
    ++g_loader_calls;
    memcpy((void *)&g_seen_offset, record, sizeof g_seen_offset);
    memcpy((void *)&g_seen_size, (const unsigned char *)record + 4u,
           sizeof g_seen_size);
    memcpy((unsigned char *)record + HYPRLODR_RECORD_OBJECT_FIELD,
           &(uintptr_t){0x123456789abcdef0ull}, sizeof(uintptr_t));
    return ((uintptr_t)(*(uint32_t *)context) << 32u) | flags;
}

__declspec(dllexport) uintptr_t startup_fixture_callsite(void *, void *,
                                                          const void *, uint32_t);

typedef struct {
    unsigned char bytes[32];
} fixture_string;

static fixture_string make_name(const char *text) {
    fixture_string name;
    memset(&name, 0, sizeof name);
    memcpy(name.bytes, text, 5u);
    uint64_t size = 5u, capacity = 15u;
    memcpy(name.bytes + 0x10u, &size, sizeof size);
    memcpy(name.bytes + 0x18u, &capacity, sizeof capacity);
    return name;
}

static void init_record(unsigned char *record, uint32_t tag) {
    memset(record, 0, HYPRLODR_GAME_RECORD_SIZE);
    uint32_t offset = 295758784u, size = 21972u;
    memcpy(record, &offset, sizeof offset);
    memcpy(record + 4u, &size, sizeof size);
    memcpy(record + HYPRLODR_RECORD_TAG_FIELD, &tag, sizeof tag);
}

static int fail(const char *message) {
    printf("preentry fixture: FAIL: %s\n", message);
    return 1;
}

#if !defined(HYPRLODR_FIXTURE_DYNAMIC)
static int bytes_equal(const unsigned char *left, const unsigned char *right,
                       size_t length) {
    for (size_t index = 0; index < length; ++index)
        if (left[index] != right[index]) return 0;
    return 1;
}
#endif

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t marker = 0x7654u;
    unsigned char record[HYPRLODR_GAME_RECORD_SIZE];
    fixture_string hand2 = make_name("hand2");
    uintptr_t expected_original = ((uintptr_t)marker << 32u) | 0x11u;

#if defined(HYPRLODR_FIXTURE_DYNAMIC)
    /* Exercise the original instruction before LoadLibrary. A late process
     * attach must reject installation and leave the callsite untouched. */
    (void)startup_fixture_callsite(&marker, record, &hand2, 0x11u);
    HMODULE module = LoadLibraryA("hyprlodr_startup_fixture.dll");
    if (!module) return fail("late LoadLibrary failed");
    int (*get_status)(void) = (int (*)(void))GetProcAddress(
        module, "startup_fixture_hook_status");
    if (!get_status || get_status() != 2) return fail("dynamic attach was not rejected");
    init_record(record, 1u);
    if (startup_fixture_callsite(&marker, record, &hand2, 0x11u) != expected_original ||
        g_loader_calls != 2 || g_seen_offset != 295758784u || g_seen_size != 21972u)
        return fail("late load modified original call behavior");
    printf("preentry fixture: dynamic load rejected; original CALL remained intact\n");
    return 0;
#else
    int status = startup_fixture_hook_status();
    if (status == 1) {
        init_record(record, 1u);
        if (startup_fixture_callsite(&marker, record, &hand2, 0x11u) != expected_original ||
            g_loader_calls != 1 || g_seen_offset != 295758784u || g_seen_size != 21972u)
            return fail("dormant pre-entry hook did not forward unchanged");
        void *site = startup_fixture_hook_site();
        if (!site) return fail("installed hook has no site address");
        unsigned char installed_bytes[5];
        memcpy(installed_bytes, site, sizeof installed_bytes);

        char skip[8];
        if (GetEnvironmentVariableA("HYPRLODR_FIXTURE_SKIP_ROUTE", skip,
                                    (DWORD)sizeof skip) != 1u || skip[0] != '1') {
            startup_fixture_enable();
            init_record(record, 1u);
            (void)startup_fixture_callsite(&marker, record, &hand2, 0x22u);
            if (g_seen_offset != 306794369u || g_seen_size != 22056u ||
                g_loader_calls != 2 ||
                *(uintptr_t *)(record + HYPRLODR_RECORD_OBJECT_FIELD) !=
                    (uintptr_t)0x123456789abcdef0ull)
                return fail("deferred route publication did not substitute and restore");
            init_record(record, 2u);
            (void)startup_fixture_callsite(&marker, record, &hand2, 0x33u);
            if (g_seen_offset != 295758784u || g_seen_size != 21972u)
                return fail("same-name tag-2 resource was routed");
            init_record(record, 1u);
            fixture_string unrelated = make_name("other");
            (void)startup_fixture_callsite(&marker, record, &unrelated, 0x44u);
            if (g_seen_offset != 295758784u || g_seen_size != 21972u)
                return fail("unrelated resource was routed");
            startup_fixture_disable();
            init_record(record, 1u);
            (void)startup_fixture_callsite(&marker, record, &hand2, 0x55u);
            if (g_seen_offset != 295758784u || g_seen_size != 21972u)
                return fail("logical disable did not restore pass-through");
            unsigned char after_disable[5];
            memcpy(after_disable, site, sizeof after_disable);
            if (!bytes_equal(installed_bytes, after_disable, sizeof installed_bytes))
                return fail("logical disable rewrote executable code");
        } else {
            init_record(record, 1u);
            (void)startup_fixture_callsite(&marker, record, &hand2, 0x66u);
            if (g_seen_offset != 295758784u || g_seen_size != 21972u)
                return fail("failed bootstrap changed dormant behavior");
        }
    } else if (status != 0 && status != 3 && status != 4) {
        return fail("unexpected process-start install status");
    }
    if (status != 1) {
        init_record(record, 1u);
        (void)startup_fixture_callsite(&marker, record, &hand2, 0x77u);
        if (g_seen_offset != 295758784u || g_seen_size != 21972u)
            return fail("rejected installation changed original code");
    }
    printf("preentry fixture: attach=%d dormant forwarding, deferred route, disable and lifetime passed\n",
           status);
    return 0;
#endif
}
