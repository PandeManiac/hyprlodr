#define HYPRLODR_MINIMAL_WIN32 1
#include "hypr_win32.h"
#include "startup_diag.h"

#include <stdio.h>
#include <string.h>

#define HYPRLODR_STARTUP_DIAG_CAPACITY 128u
#define HYPRLODR_STARTUP_DIAG_TEXT 512u

typedef struct {
    volatile LONG ready;
    uint32_t sequence;
    DWORD tick;
    DWORD thread;
    uint32_t kind;
    uint32_t tag;
    uint32_t offset;
    uint32_t size;
    uint32_t gate_state;
    uint32_t flags;
    uintptr_t result;
    uintptr_t parsed_object;
    char text[HYPRLODR_STARTUP_DIAG_TEXT];
} diag_event;

static volatile LONG g_enabled;
static volatile LONG g_reserved;
static volatile LONG g_dropped;
static volatile LONG g_stop;
static volatile LONG g_once_mask;
static volatile LONG g_drain_lock;
static uint32_t g_drained;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static HANDLE g_writer_thread;
static diag_event g_events[HYPRLODR_STARTUP_DIAG_CAPACITY];

static DWORD WINAPI diagnostic_writer(LPVOID unused);

static const char *kind_name(uint32_t kind) {
    static const char *const names[HYPRLODR_DIAG_KIND_COUNT] = {
        "early_hook_install", "file_hook_install", "bootstrap_start",
        "first_archive_attempt", "archive_gate_wait", "archive_open",
        "redirect_publication", "route_preflight", "route_activation",
        "archive_gate_release", "first_loader_callsite", "first_hand2_request",
        "first_tag1_hand2_request", "hand2_loader_call", "hand2_loader_return",
        "tag1_hand2_loader_call", "tag1_hand2_loader_return"
    };
    return kind < HYPRLODR_DIAG_KIND_COUNT ? names[kind] : "unknown";
}

static void copy_text(char destination[HYPRLODR_STARTUP_DIAG_TEXT], const char *source) {
    size_t length = 0;
    if (source) {
        while (length + 1u < HYPRLODR_STARTUP_DIAG_TEXT && source[length]) {
            destination[length] = source[length];
            ++length;
        }
    }
    destination[length] = '\0';
}

void hyprlodr_startup_diag_enable(int enabled) {
    __atomic_store_n(&g_enabled, enabled ? 1l : 0l, __ATOMIC_RELEASE);
}

int hyprlodr_startup_diag_enabled(void) {
    return __atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE) != 0;
}

void hyprlodr_startup_diag_emit(hyprlodr_startup_diag_kind kind,
                                uint32_t tag, uint32_t offset, uint32_t size,
                                uint32_t gate_state, uint32_t flags,
                                uintptr_t result, uintptr_t parsed_object,
                                const char *text) {
    if (!__atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE) ||
        (unsigned)kind >= HYPRLODR_DIAG_KIND_COUNT) return;
    LONG index = __atomic_fetch_add(&g_reserved, 1l, __ATOMIC_RELAXED);
    if ((unsigned long)index >= HYPRLODR_STARTUP_DIAG_CAPACITY) {
        __atomic_add_fetch(&g_dropped, 1l, __ATOMIC_RELAXED);
        return;
    }
    diag_event *event = &g_events[index];
    event->sequence = (uint32_t)index + 1u;
    event->tick = GetTickCount();
    event->thread = GetCurrentThreadId();
    event->kind = (uint32_t)kind;
    event->tag = tag;
    event->offset = offset;
    event->size = size;
    event->gate_state = gate_state;
    event->flags = flags;
    event->result = result;
    event->parsed_object = parsed_object;
    copy_text(event->text, text);
    __atomic_store_n(&event->ready, 1l, __ATOMIC_RELEASE);
}

void hyprlodr_startup_diag_emit_once(hyprlodr_startup_diag_kind kind,
                                     uint32_t tag, uint32_t offset, uint32_t size,
                                     uint32_t gate_state, uint32_t flags,
                                     uintptr_t result, uintptr_t parsed_object,
                                     const char *text) {
    if (!__atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE) || (unsigned)kind >= 32u) return;
    LONG bit = (LONG)(1u << (unsigned)kind);
    LONG old = __atomic_fetch_or(&g_once_mask, bit, __ATOMIC_ACQ_REL);
    if ((old & bit) == 0)
        hyprlodr_startup_diag_emit(kind, tag, offset, size, gate_state, flags,
                                   result, parsed_object, text);
}

static void write_event(const diag_event *event) {
    char line[1200];
    char escaped[HYPRLODR_STARTUP_DIAG_TEXT * 2u];
    size_t used = 0;
    DWORD written = 0;
    for (const char *at = event->text; *at && used + 2u < sizeof escaped; ++at) {
        if (*at == '\\' || *at == '"') escaped[used++] = '\\';
        if ((unsigned char)*at >= 0x20u) escaped[used++] = *at;
    }
    escaped[used] = '\0';
    int length = snprintf(line, sizeof line,
        "{\"seq\":%u,\"t_ms\":%lu,\"thread\":%lu,\"event\":\"%s\","
        "\"tag\":%u,\"offset\":%u,\"size\":%u,\"gate_state\":%u,\"flags\":%u,"
        "\"result\":\"0x%llx\",\"parsed_object\":\"0x%llx\",\"text\":\"%s\"}\n",
        event->sequence, (unsigned long)event->tick, (unsigned long)event->thread,
        kind_name(event->kind), event->tag, event->offset, event->size,
        event->gate_state, event->flags,
        (unsigned long long)event->result,
        (unsigned long long)event->parsed_object, escaped);
    if (length > 0 && (size_t)length < sizeof line)
        WriteFile(g_log, line, (DWORD)length, &written, NULL);
}

int hyprlodr_startup_diag_start(const char *path) {
    if (!__atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE)) return 0;
    if (g_log != INVALID_HANDLE_VALUE) return g_writer_thread ? 0 : -1;
    if (!path || !*path) return -1;
    HANDLE log = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE) return -1;
    g_log = log;
    hyprlodr_startup_diag_drain();
    hyprlodr_startup_diag_emit(HYPRLODR_DIAG_BOOTSTRAP_START, 0u, 0u, 0u,
                               0u, 0u, 0u, 0u, "bootstrap worker started");
    hyprlodr_startup_diag_drain();
    g_writer_thread = CreateThread(NULL, 0, diagnostic_writer, NULL, 0, NULL);
    return g_writer_thread ? 0 : -1;
}

void hyprlodr_startup_diag_drain(void) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    /* Bootstrap can report milestones while the writer is draining the same
     * ring. Keep the single-consumer cursor and file writes serialized without
     * making producers wait. A contending drain is picked up on the next pass. */
    if (__atomic_exchange_n(&g_drain_lock, 1l, __ATOMIC_ACQUIRE) != 0) return;
    LONG reserved = __atomic_load_n(&g_reserved, __ATOMIC_ACQUIRE);
    uint32_t limit = reserved < 0 ? 0u : (uint32_t)reserved;
    if (limit > HYPRLODR_STARTUP_DIAG_CAPACITY) limit = HYPRLODR_STARTUP_DIAG_CAPACITY;
    while (g_drained < limit) {
        diag_event *event = &g_events[g_drained];
        if (!__atomic_load_n(&event->ready, __ATOMIC_ACQUIRE)) break;
        write_event(event);
        ++g_drained;
    }
    LONG dropped = __atomic_exchange_n(&g_dropped, 0l, __ATOMIC_ACQ_REL);
    if (dropped > 0) {
        char line[160];
        DWORD written = 0;
        int length = snprintf(line, sizeof line,
                              "{\"seq\":%u,\"event\":\"diagnostic_events_dropped\",\"count\":%ld}\n",
                              limit + 1u, dropped);
        if (length > 0) WriteFile(g_log, line, (DWORD)length, &written, NULL);
    }
    __atomic_store_n(&g_drain_lock, 0l, __ATOMIC_RELEASE);
}

void hyprlodr_startup_diag_request_stop(void) {
    __atomic_store_n(&g_stop, 1l, __ATOMIC_RELEASE);
}

void hyprlodr_startup_diag_pump(void) {
    if (!__atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE)) return;
    while (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE)) {
        Sleep(50u);
        hyprlodr_startup_diag_drain();
    }
    hyprlodr_startup_diag_drain();
}

static DWORD WINAPI diagnostic_writer(LPVOID unused) {
    (void)unused;
    hyprlodr_startup_diag_pump();
    return 0;
}
