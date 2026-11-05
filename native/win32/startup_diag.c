#define HYPRLODR_MINIMAL_WIN32 1
#include "hypr_win32.h"
#include "startup_diag.h"

#include <stdio.h>
#include <string.h>

#define HYPRLODR_STARTUP_DIAG_CAPACITY 1024u
#define HYPRLODR_STARTUP_DIAG_TEXT 512u

typedef struct {
    volatile LONG ready;
    uint32_t sequence;
    uint32_t process_id;
    uint64_t run_id;
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
static volatile LONG g_total_dropped;
static volatile LONG g_stop;
static volatile LONG g_once_mask;
static volatile LONG g_drain_lock;
static volatile LONG g_writer_status;
static volatile LONG g_sequence;
#if defined(HYPRLODR_STARTUP_DIAG_TESTING)
static volatile LONG g_test_fail_writer;
#endif
static uint64_t g_run_id;
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
        "tag1_hand2_loader_call", "tag1_hand2_loader_return", "diagnostic_writer_status"
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

void hyprlodr_startup_diag_set_run_id(uint64_t run_id) {
    __atomic_store_n(&g_run_id, run_id, __ATOMIC_RELEASE);
}

uint64_t hyprlodr_startup_diag_run_id(void) {
    return __atomic_load_n(&g_run_id, __ATOMIC_ACQUIRE);
}

uint32_t hyprlodr_startup_diag_next_sequence(void) {
    return (uint32_t)__atomic_add_fetch(&g_sequence, 1l, __ATOMIC_RELAXED);
}

uint32_t hyprlodr_startup_diag_dropped(void) {
    return (uint32_t)__atomic_load_n(&g_total_dropped, __ATOMIC_ACQUIRE);
}

int hyprlodr_startup_diag_writer_status(void) {
    return (int)__atomic_load_n(&g_writer_status, __ATOMIC_ACQUIRE);
}

#if defined(HYPRLODR_STARTUP_DIAG_TESTING)
void hyprlodr_startup_diag_test_fail_next_writer(void) {
    __atomic_store_n(&g_test_fail_writer, 1l, __ATOMIC_RELEASE);
}
#endif

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
        __atomic_add_fetch(&g_total_dropped, 1l, __ATOMIC_RELAXED);
        return;
    }
    diag_event *event = &g_events[index];
    event->sequence = hyprlodr_startup_diag_next_sequence();
    event->process_id = GetCurrentProcessId();
    event->run_id = hyprlodr_startup_diag_run_id();
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
        "{\"seq\":%u,\"run_id\":\"%016llx\",\"pid\":%lu,\"t_ms\":%lu,\"thread\":%lu,\"event\":\"%s\","
        "\"tag\":%u,\"offset\":%u,\"size\":%u,\"gate_state\":%u,\"flags\":%u,"
        "\"result\":\"0x%llx\",\"parsed_object\":\"0x%llx\",\"text\":\"%s\"}\n",
        event->sequence, (unsigned long long)event->run_id,
        (unsigned long)event->process_id, (unsigned long)event->tick,
        (unsigned long)event->thread,
        kind_name(event->kind), event->tag, event->offset, event->size,
        event->gate_state, event->flags,
        (unsigned long long)event->result,
        (unsigned long long)event->parsed_object, escaped);
    if (length > 0 && (size_t)length < sizeof line)
        WriteFile(g_log, line, (DWORD)length, &written, NULL);
}

int hyprlodr_startup_diag_start(const char *path) {
    if (!__atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_writer_status, 4l, __ATOMIC_RELEASE); /* disabled */
        return 0;
    }
    if (__atomic_load_n(&g_writer_status, __ATOMIC_ACQUIRE) == 5l) return -1;
    if (g_log != INVALID_HANDLE_VALUE) return g_writer_thread ? 0 : -1;
    if (!path || !*path) {
        __atomic_store_n(&g_writer_status, 2l, __ATOMIC_RELEASE); /* invalid path */
        return -1;
    }
    HANDLE log = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE) {
        __atomic_store_n(&g_writer_status, 2l, __ATOMIC_RELEASE); /* log open failed */
        return -1;
    }
    g_log = log;
    hyprlodr_startup_diag_drain();
    hyprlodr_startup_diag_emit(HYPRLODR_DIAG_BOOTSTRAP_START, 0u, 0u, 0u,
                               0u, 0u, 0u, 0u, "bootstrap worker started");
    hyprlodr_startup_diag_emit(HYPRLODR_DIAG_WRITER_STATUS, 0u, 0u, 0u,
                               0u, 1u, 0u, 0u, "writer starting");
    hyprlodr_startup_diag_drain();
#if defined(HYPRLODR_STARTUP_DIAG_TESTING)
    if (__atomic_exchange_n(&g_test_fail_writer, 0l, __ATOMIC_ACQ_REL))
        g_writer_thread = NULL;
    else
#endif
        g_writer_thread = CreateThread(NULL, 0, diagnostic_writer, NULL, 0, NULL);
    if (!g_writer_thread) {
        __atomic_store_n(&g_writer_status, 3l, __ATOMIC_RELEASE); /* thread creation failed */
        hyprlodr_startup_diag_emit(HYPRLODR_DIAG_WRITER_STATUS, 0u, 0u, 0u,
                                   0u, 0u, 0u, 0u, "writer thread creation failed");
        hyprlodr_startup_diag_drain();
        return -1;
    }
    __atomic_store_n(&g_writer_status, 1l, __ATOMIC_RELEASE); /* running */
    hyprlodr_startup_diag_emit(HYPRLODR_DIAG_WRITER_STATUS, 0u, 0u, 0u,
                               0u, 2u, 0u, 0u, "writer running");
    return 0;
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
                              "{\"seq\":%u,\"run_id\":\"%016llx\",\"pid\":%lu,"
                              "\"event\":\"diagnostic_events_dropped\",\"count\":%ld}\n",
                              hyprlodr_startup_diag_next_sequence(),
                              (unsigned long long)hyprlodr_startup_diag_run_id(),
                              (unsigned long)GetCurrentProcessId(), dropped);
        if (length > 0) WriteFile(g_log, line, (DWORD)length, &written, NULL);
    }
    __atomic_store_n(&g_drain_lock, 0l, __ATOMIC_RELEASE);
}

void hyprlodr_startup_diag_request_stop(void) {
    __atomic_store_n(&g_stop, 1l, __ATOMIC_RELEASE);
}

int hyprlodr_startup_diag_finish(uint32_t timeout_ms) {
    hyprlodr_startup_diag_request_stop();
    if (g_writer_thread) {
        if (WaitForSingleObject(g_writer_thread, timeout_ms) != 0u) return -1;
        CloseHandle(g_writer_thread);
        g_writer_thread = NULL;
    }
    hyprlodr_startup_diag_drain();
    if (g_log != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
    __atomic_store_n(&g_writer_status, 5l, __ATOMIC_RELEASE); /* stopped */
    return 0;
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
